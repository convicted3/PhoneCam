package com.phonecam.app;

import android.content.Context;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Range;
import android.util.Size;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;

/** Opens a camera, feeds it into a hardware H.264 encoder and hands out encoded packets. */
final class CameraStreamer {
    private static final String TAG = "PhoneCam";

    interface Sink {
        void onPacket(int type, long ptsUs, byte[] data);
        void onStreamError(String message);
    }

    static final class Request {
        int width = 1280, height = 720, fps = 30, kbps = 6000;
        String camera = "back";
        boolean highSpeed;   // experimental: reach the fps through a 120 fps high-speed session
    }

    static final class Started {
        int width, height, fps, maxFps;
        String note = "";
        String cameraId;
        int sensorOrientation;
        boolean front;
        Size previewSize; // null if no preview
    }

    private final Context ctx;
    private final CameraManager manager;
    private HandlerThread camThread;
    private Handler camHandler;

    private CameraDevice device;
    private CameraCaptureSession session;
    private CaptureRequest.Builder builder;
    private MediaCodec encoder;
    private Surface encoderSurface;
    private Surface previewSurface;
    private Thread drainThread;
    private GlRelay relay;
    private android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession hsSession;
    private volatile boolean running;
    private volatile Sink sink;

    private Rect activeArray;
    private float maxZoom = 1f;
    private float zoom = 1f;
    private float baseZoom = 1f;   // 0.6 etc. for ultra-wide through zoom ratio
    private boolean torch;
    private boolean hasFlash;
    private boolean hasContinuousAf;
    private CameraCharacteristics currentChars;

    /** Frame counters for the on-screen / PC statistics. */
    final AtomicInteger cameraFrames = new AtomicInteger();
    final AtomicInteger encodedFrames = new AtomicInteger();
    final AtomicInteger captureFailures = new AtomicInteger();
    volatile String fpsRangeText = "?";
    volatile String encoderInfo = "?";
    private volatile boolean realtimeTimestamps;
    private volatile boolean clockChecked;

    /** "Now" in the same clock as the camera/encoder timestamps, in microseconds. */
    long nowUs() {
        return (realtimeTimestamps ? android.os.SystemClock.elapsedRealtimeNanos() : System.nanoTime()) / 1000;
    }
    private boolean intraRefresh;

    private final CameraCaptureSession.CaptureCallback countFrames = new CameraCaptureSession.CaptureCallback() {
        @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult res) {
            cameraFrames.incrementAndGet();
            Long t = res.get(CaptureResult.SENSOR_EXPOSURE_TIME);
            Integer iso = res.get(CaptureResult.SENSOR_SENSITIVITY);
            if (t != null && iso != null && t > 0 && iso > 0) exposureProduct = (double) t * iso;
            Integer afs = res.get(CaptureResult.CONTROL_AF_STATE);
            if (afs != null) afState = afs;
            Integer fs = res.get(CaptureResult.FLASH_STATE);
            flashState = fs != null ? fs : -1;
            if (torch) {
                torchFrames++;
                if (fs != null) torchStateReported = true;
                if (fs != null && fs == CaptureResult.FLASH_STATE_FIRED) torchSeenLit = true;
                // the camera processed the request with the torch on: count that as lit too
                Integer fmr = res.get(CaptureResult.FLASH_MODE);
                if (fmr != null && fmr == CaptureRequest.FLASH_MODE_TORCH && torchFrames > 10) torchSeenLit = true;
            }
        }
        @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest r,
                                              android.hardware.camera2.CaptureFailure f) {
            captureFailures.incrementAndGet();
        }
    };

    CameraStreamer(Context ctx) {
        this.ctx = ctx.getApplicationContext();
        this.manager = (CameraManager) this.ctx.getSystemService(Context.CAMERA_SERVICE);
    }

    synchronized boolean isRunning() { return running; }

    /** Pick a camera id for "back", "front" or a literal id. */
    String findCameraId(String which) throws CameraAccessException {
        String[] ids = manager.getCameraIdList();
        for (String id : ids) if (id.equals(which)) return id;
        int want = "front".equalsIgnoreCase(which)
                ? CameraCharacteristics.LENS_FACING_FRONT : CameraCharacteristics.LENS_FACING_BACK;
        for (String id : ids) {
            Integer f = manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
            if (f != null && f == want) return id;
        }
        return ids.length > 0 ? ids[0] : null;
    }

    static final class Choice {
        String id;
        CameraCharacteristics cc;
        Size size;
        int maxFps;              // what this camera can really do at this size
        Range<Integer> range;    // AE fps range we will request
        float baseZoom = 1f;     // < 1 = ultra-wide through the main camera's zoom ratio (Android 11+)
    }

    @SuppressWarnings("unchecked")
    private static Range<Float> zoomRatioRange(CameraCharacteristics cc) {
        if (Build.VERSION.SDK_INT < 30 || cc == null) return null;
        try {
            Object o = cc.get((CameraCharacteristics.Key<?>) CameraCharacteristics.class
                    .getField("CONTROL_ZOOM_RATIO_RANGE").get(null));
            return o instanceof Range ? (Range<Float>) o : null;
        } catch (Throwable t) {
            return null;
        }
    }

    private static float focal(CameraCharacteristics cc) {
        float[] f = cc.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
        return f != null && f.length > 0 ? f[0] : 0f;
    }

    /**
     * Ultra-wide: preferably the main back camera with a zoom ratio below 1 (keeps autofocus, lens switching and
     * the high-speed mode); otherwise a separate back camera with a clearly shorter focal length. null = none.
     */
    private Choice chooseWide(Request r) throws CameraAccessException {
        Request q = new Request();
        q.width = r.width; q.height = r.height; q.fps = r.fps; q.kbps = r.kbps; q.camera = "back";
        Choice main = chooseCamera(q);
        if (main == null) return null;
        Range<Float> zr = zoomRatioRange(main.cc);
        if (zr != null && zr.getLower() < 0.95f) {
            main.baseZoom = zr.getLower();
            return main;
        }
        float mainFocal = focal(main.cc);
        Choice best = null;
        float bestFocal = Float.MAX_VALUE;
        for (String id : allCameraIds()) {
            if (id.equals(main.id)) continue;
            CameraCharacteristics cc;
            try { cc = manager.getCameraCharacteristics(id); } catch (Exception e) { continue; }
            Integer f = cc.get(CameraCharacteristics.LENS_FACING);
            if (f == null || f != CameraCharacteristics.LENS_FACING_BACK || !isNormalCamera(cc)) continue;
            float fl = focal(cc);
            if (fl <= 0 || mainFocal <= 0 || fl > mainFocal * 0.8f || fl >= bestFocal) continue;
            StreamConfigurationMap map = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            if (map == null) continue;
            Choice c = new Choice();
            c.id = id;
            c.cc = cc;
            c.size = chooseSize(map.getOutputSizes(MediaCodec.class), r.width, r.height, 3840);
            c.maxFps = maxFpsAt(cc, c.size);
            c.range = chooseFpsRange(cc, Math.min(r.fps, c.maxFps > 0 ? c.maxFps : r.fps));
            best = c;
            bestFocal = fl;
        }
        return best;
    }

    /** All camera ids, including ones some phones don't list (e.g. extra lenses at "2", "3"...). */
    private List<String> allCameraIds() {
        List<String> ids = new ArrayList<String>();
        try { for (String id : manager.getCameraIdList()) ids.add(id); } catch (Exception ignored) {}
        for (int i = 0; i < 16; i++) {
            String id = String.valueOf(i);
            if (ids.contains(id)) continue;
            try {
                manager.getCameraCharacteristics(id);
                ids.add(id);
            } catch (Throwable ignored) {}
        }
        return ids;
    }

    private static boolean isNormalCamera(CameraCharacteristics cc) {
        int[] caps = cc.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        if (caps == null) return true;
        for (int c : caps) if (c == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE) return true;
        return false;
    }

    /** Max fps a camera can deliver to the encoder at this size in a normal session. */
    private static int maxFpsAt(CameraCharacteristics cc, Size size) {
        Range<Integer>[] ranges = cc.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        int maxUpper = 30;
        if (ranges != null) {
            maxUpper = 0;
            for (Range<Integer> rg : ranges) maxUpper = Math.max(maxUpper, rg.getUpper());
        }
        StreamConfigurationMap map = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if (map != null) {
            try {
                long d = map.getOutputMinFrameDuration(MediaCodec.class, size);
                if (d > 0) maxUpper = Math.min(maxUpper, (int) Math.round(1e9 / d));
            } catch (Exception ignored) {}
        }
        return maxUpper;
    }

    /**
     * Pick the camera for the request: the default camera of the requested side if it can do the requested fps,
     * otherwise another camera of the same side that can, otherwise the default one at its best fps.
     */
    Choice chooseCamera(Request r) throws CameraAccessException {
        String which = r.camera;
        if ("wide".equalsIgnoreCase(which)) return chooseWide(r);
        int want = "front".equalsIgnoreCase(which)
                ? CameraCharacteristics.LENS_FACING_FRONT : CameraCharacteristics.LENS_FACING_BACK;
        List<Choice> candidates = new ArrayList<Choice>();
        for (String id : allCameraIds()) {
            CameraCharacteristics cc;
            try { cc = manager.getCameraCharacteristics(id); } catch (Exception e) { continue; }
            boolean literal = id.equals(which);
            Integer f = cc.get(CameraCharacteristics.LENS_FACING);
            if (!literal && (f == null || f != want)) continue;
            if (!isNormalCamera(cc)) continue;
            StreamConfigurationMap map = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            if (map == null) continue;
            Choice c = new Choice();
            c.id = id;
            c.cc = cc;
            c.size = chooseSize(map.getOutputSizes(MediaCodec.class), r.width, r.height, 3840);
            c.maxFps = maxFpsAt(cc, c.size);
            if (literal) { candidates.clear(); candidates.add(c); break; }
            candidates.add(c);
        }
        if (candidates.isEmpty()) {
            String[] ids = manager.getCameraIdList();
            if (ids.length == 0) return null;
            Choice c = new Choice();
            c.id = ids[0];
            c.cc = manager.getCameraCharacteristics(c.id);
            StreamConfigurationMap map = c.cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            c.size = chooseSize(map != null ? map.getOutputSizes(MediaCodec.class) : null, r.width, r.height, 3840);
            c.maxFps = maxFpsAt(c.cc, c.size);
            candidates.add(c);
        }
        Choice pick = candidates.get(0); // default camera of that side
        if (pick.maxFps < r.fps) {
            for (Choice c : candidates) {
                boolean sameSize = c.size.getWidth() == pick.size.getWidth() && c.size.getHeight() == pick.size.getHeight();
                if (c.maxFps >= r.fps && sameSize) { pick = c; break; }
            }
        }
        int fps = Math.min(r.fps, pick.maxFps > 0 ? pick.maxFps : r.fps);
        pick.range = chooseFpsRange(pick.cc, fps);
        Log.i(TAG, "camera " + pick.id + " size " + pick.size + " max " + pick.maxFps + " fps, range " + pick.range);
        return pick;
    }

    /**
     * "PHC1 CAPS side/requested/actual/maxFps/hsFps ..." for back and front cameras at the PC's resolutions.
     * maxFps = normal mode limit, hsFps = 60 if 60 fps is reachable through the high-speed mode, else 0.
     */
    String capabilities() {
        StringBuilder sb = new StringBuilder("PHC1 CAPS");
        int[][] res = {{640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160}};
        for (String side : new String[]{"back", "wide", "front"}) {
            int want = side.equals("front") ? CameraCharacteristics.LENS_FACING_FRONT : CameraCharacteristics.LENS_FACING_BACK;
            for (int[] rr : res) {
                try {
                    Request q = new Request();
                    q.width = rr[0]; q.height = rr[1]; q.fps = 60; q.camera = side;
                    Choice c = chooseCamera(q);
                    if (c == null) continue;
                    Integer f = c.cc.get(CameraCharacteristics.LENS_FACING);
                    if (f == null || f != want) continue;
                    HighSpeedPlan hp = c.maxFps >= 60 ? null : highSpeedPlan(c.cc, c.size, 60);
                    sb.append(' ').append(side).append('/').append(rr[0]).append('x').append(rr[1]).append('/')
                            .append(c.size.getWidth()).append('x').append(c.size.getHeight()).append('/')
                            .append(c.maxFps).append('/').append(hp != null ? 60 : 0);
                } catch (Exception e) {
                    Log.w(TAG, "caps", e);
                }
            }
        }
        return sb.toString();
    }

    CameraCharacteristics characteristics(String id) throws CameraAccessException {
        return manager.getCameraCharacteristics(id);
    }

    private static Size chooseSize(Size[] sizes, int w, int h, int maxW) {
        if (sizes == null || sizes.length == 0) return new Size(w, h);
        double want = (double) w / h;
        Size best = null;
        long bestScore = Long.MAX_VALUE;
        for (Size s : sizes) {
            if (s.getWidth() > maxW) continue;
            if ((s.getWidth() & 15) != 0 && (s.getWidth() & 7) != 0) continue;
            double ar = (double) s.getWidth() / s.getHeight();
            long areaDiff = Math.abs((long) s.getWidth() * s.getHeight() - (long) w * h);
            long score = areaDiff + (Math.abs(ar - want) > 0.02 ? 100000000L : 0);
            if (s.getWidth() == w && s.getHeight() == h) score = -1;
            if (score < bestScore) { bestScore = score; best = s; }
        }
        return best != null ? best : sizes[0];
    }

    /** Lowest H.264 level that allows this size/fps/bitrate (Baseline/Main limits). */
    private static int avcLevel(int w, int h, int fps, int kbps) {
        long mbs = (long) ((w + 15) / 16) * ((h + 15) / 16);
        long mbps = mbs * fps;
        int[][] t = { // max MB/s, max frame MBs, max kbps, level constant
                {108000, 3600, 14000, 0x200},   // 3.1
                {216000, 5120, 20000, 0x400},   // 3.2
                {245760, 8192, 20000, 0x800},   // 4.0
                {245760, 8192, 50000, 0x1000},  // 4.1
                {522240, 8704, 50000, 0x2000},  // 4.2
                {589824, 22080, 135000, 0x4000},// 5.0
                {983040, 36864, 240000, 0x8000},// 5.1
                {2073600, 36864, 240000, 0x10000}, // 5.2
        };
        for (int[] l : t) if (mbps <= l[0] && mbs <= l[1] && kbps <= l[2]) return l[3];
        return 0x10000;
    }

    private MediaCodec createEncoder(int w, int h, int fps, int kbps) throws Exception {
        return createEncoder(w, h, fps, kbps, 0);
    }

    /** maxFpsToEncoder > 0: the framework drops input frames above this rate (API 29+, for 120 fps capture). */
    private MediaCodec createEncoder(int w, int h, int fps, int kbps, int maxFpsToEncoder) throws Exception {
        Exception last = null;
        int level = avcLevel(w, h, fps, kbps);
        int[] profiles = {MediaCodecInfo.CodecProfileLevel.AVCProfileHigh,
                MediaCodecInfo.CodecProfileLevel.AVCProfileMain,
                MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline, -1, -2};
        for (int attempt = 0; attempt < profiles.length; attempt++) {
            int profile = profiles[attempt];
            MediaFormat f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h);
            f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            f.setInteger(MediaFormat.KEY_BIT_RATE, kbps * 1000);
            f.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
            f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
            if (profile > 0) {
                f.setInteger("profile", profile);
                f.setInteger("level", level);
            }
            if (profile != -2) {
                if (Build.VERSION.SDK_INT >= 23) {
                    f.setInteger("priority", 0);
                    f.setInteger("operating-rate", fps);
                }
                if (Build.VERSION.SDK_INT >= 29) {
                    f.setInteger("prepend-sps-pps-to-idr-frames", 1);
                    f.setInteger("max-bframes", 0);
                }
                if (Build.VERSION.SDK_INT >= 30) f.setInteger("latency", 1);
                if (maxFpsToEncoder > 0) f.setFloat("max-fps-to-encoder", maxFpsToEncoder);
            }
            MediaCodec c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            try {
                // Constant bitrate makes the stream use the quality chosen on the PC.
                MediaCodecInfo.CodecCapabilities caps =
                        c.getCodecInfo().getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC);
                // VBR (the encoder default): CBR made some encoders starve the stream to ~0 Mbit/s.
                if (profile != -2 && caps.getEncoderCapabilities() != null && caps.getEncoderCapabilities()
                        .isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR))
                    f.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR);
                // Intra refresh spreads the refresh over many frames instead of one huge key frame
                // every second: no periodic bursts on the network, much steadier latency.
                if (profile != -2 && Build.VERSION.SDK_INT >= 24
                        && caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_IntraRefresh)) {
                    f.setInteger("intra-refresh-period", fps);
                    f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 10);
                    intraRefresh = true;
                } else {
                    intraRefresh = false;
                }
                if (profile > 0) {
                    boolean supported = false;
                    for (MediaCodecInfo.CodecProfileLevel pl : caps.profileLevels)
                        if (pl.profile == profile) supported = true;
                    if (!supported) { c.release(); continue; }
                }
                c.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                encoderInfo = c.getName() + " " + (profile == 8 ? "High" : profile == 2 ? "Main" : profile == 1 ? "Baseline" : "auto")
                        + L.t(", цель ") + (kbps / 1000.0) + L.t(" Мбит/с") + (intraRefresh ? ", intra-refresh" : "");
                Log.i(TAG, "encoder " + encoderInfo + " " + w + "x" + h + "@" + fps + " level 0x" + Integer.toHexString(level));
                return c;
            } catch (Exception e) {
                last = e;
                Log.w(TAG, "encoder configure attempt " + attempt + " failed", e);
                c.release();
            }
        }
        throw last != null ? last : new Exception(L.t("кодер H.264 не настроился"));
    }

    static final class HighSpeedPlan { Range<Integer> range; int keepEvery; }

    /** Find a high-speed range at this size whose rate is a whole multiple of the wanted fps. */
    private static HighSpeedPlan highSpeedPlan(CameraCharacteristics cc, Size size, int fps) {
        if (Build.VERSION.SDK_INT < 23) return null;
        int[] caps = cc.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        boolean hs = false;
        if (caps != null) for (int c : caps)
            if (c == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO) hs = true;
        StreamConfigurationMap map = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if (!hs || map == null) return null;
        boolean sizeOk = false;
        for (Size s : map.getHighSpeedVideoSizes()) if (s.equals(size)) sizeOk = true;
        if (!sizeOk) return null;
        HighSpeedPlan best = null;
        for (Range<Integer> rg : map.getHighSpeedVideoFpsRangesFor(size)) {
            int up = rg.getUpper();
            if (up < fps || up % fps != 0) continue;
            boolean fixed = rg.getLower().equals(rg.getUpper());
            // prefer fixed ranges, then the lowest multiple (120 over 240)
            if (best == null
                    || (fixed && !best.range.getLower().equals(best.range.getUpper()))
                    || (fixed == best.range.getLower().equals(best.range.getUpper()) && up < best.range.getUpper())) {
                best = new HighSpeedPlan();
                best.range = rg;
                best.keepEvery = up / fps;
            }
        }
        return best;
    }

    /** Blocking: opens camera + encoder. previewTex may be null. */
    synchronized Started start(Request r, SurfaceTexture previewTex, Sink sink) throws Exception {
        stopLocked();
        this.sink = sink;
        zoom = 1f;
        torch = false;
        baseZoom = 1f;

        camThread = new HandlerThread("cam");
        camThread.start();
        camHandler = new Handler(camThread.getLooper());

        Choice choice = chooseCamera(r);
        if (choice == null) throw new Exception(L.t("на телефоне нет камеры"));
        String id = choice.id;
        CameraCharacteristics cc = choice.cc;
        currentChars = cc;
        baseZoom = choice.baseZoom;
        StreamConfigurationMap map = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        activeArray = cc.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        Integer tsSource = cc.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
        realtimeTimestamps = tsSource != null && tsSource == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
        clockChecked = false;
        evEffective = true;
        exposureProduct = 0;
        Float mz = cc.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM);
        maxZoom = mz != null ? mz : 1f;
        Boolean fl = cc.get(CameraCharacteristics.FLASH_INFO_AVAILABLE);
        hasFlash = fl != null && fl;
        hasContinuousAf = false;
        int[] afModes = cc.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES);
        if (afModes != null) for (int m : afModes)
            if (m == CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO) hasContinuousAf = true;
        hasAutoAf = false;
        if (afModes != null) for (int m : afModes)
            if (m == CaptureRequest.CONTROL_AF_MODE_AUTO) hasAutoAf = true;

        Size size = choice.size;
        int w = size.getWidth(), h = size.getHeight();
        int fps = choice.range != null ? choice.range.getUpper() : Math.min(r.fps, 30);
        fps = Math.min(fps, r.fps);
        HighSpeedPlan hs = null;
        if (r.highSpeed && fps < r.fps) {
            hs = highSpeedPlan(cc, size, r.fps);
            if (hs != null) fps = r.fps;
        }
        String hsNote = r.highSpeed && fps < r.fps ? L.t("скоростной режим недоступен для ") + w + "x" + h : "";

        // Android 10+: camera writes 120 fps straight into the encoder, the encoder input keeps only `fps`.
        boolean hsDirect = hs != null && Build.VERSION.SDK_INT >= 29;
        MediaCodec enc;
        if (hs != null) {
            enc = createEncoder(w, h, fps, r.kbps, hsDirect ? fps : 0);
        } else {
            try {
                enc = createEncoder(w, h, fps, r.kbps);
            } catch (Exception e) {
                w = 1280; h = 720;
                enc = createEncoder(w, h, fps, r.kbps);
            }
        }
        encoder = enc;
        encoderSurface = encoder.createInputSurface();
        encoder.start();

        Started st = new Started();
        st.width = w;
        st.height = h;
        st.cameraId = id;
        st.fps = fps;
        st.maxFps = hs != null ? r.fps : choice.maxFps;
        st.note = hsNote;
        Integer so = cc.get(CameraCharacteristics.SENSOR_ORIENTATION);
        st.sensorOrientation = so != null ? so : 90;
        Integer facing = cc.get(CameraCharacteristics.LENS_FACING);
        st.front = facing != null && facing == CameraCharacteristics.LENS_FACING_FRONT;

        List<Surface> targets = new ArrayList<Surface>();
        if (hsDirect) {
            // high-speed sessions: at most one preview + one video surface, same size
            targets.add(encoderSurface);
            if (previewTex != null) {
                previewTex.setDefaultBufferSize(w, h);
                previewSurface = new Surface(previewTex);
                targets.add(previewSurface);
                st.previewSize = new Size(w, h);
            }
        } else if (hs != null) {
            // older Android: our GL relay drops frames; it is the only (preview-type) output, no on-phone preview
            relay = new GlRelay(encoderSurface, w, h, hs.keepEvery);
            targets.add(relay.start());
        } else {
            targets.add(encoderSurface);
        }
        if (hs == null && previewTex != null && map != null) {
            Size ps = chooseSize(map.getOutputSizes(SurfaceTexture.class), w, h, 1280);
            previewTex.setDefaultBufferSize(ps.getWidth(), ps.getHeight());
            previewSurface = new Surface(previewTex);
            targets.add(previewSurface);
            st.previewSize = ps;
        }

        running = true;
        startDrain();

        // open camera
        final CountDownLatch opened = new CountDownLatch(1);
        final String[] err = new String[1];
        try {
            manager.openCamera(id, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice cam) { device = cam; opened.countDown(); }
                @Override public void onDisconnected(CameraDevice cam) {
                    cam.close();
                    if (device == null) { err[0] = L.t("камера занята другим приложением"); opened.countDown(); }
                    else fail(L.t("камера отключена системой"));
                }
                @Override public void onError(CameraDevice cam, int error) {
                    cam.close();
                    if (device == null) { err[0] = L.t("ошибка открытия камеры (") + error + ")"; opened.countDown(); }
                    else fail(L.t("ошибка камеры (") + error + ")");
                }
            }, camHandler);
        } catch (SecurityException se) {
            stopLocked();
            throw new Exception(L.t("нет разрешения на камеру"));
        }
        if (!opened.await(5, TimeUnit.SECONDS) || device == null) {
            stopLocked();
            throw new Exception(err[0] != null ? err[0] : L.t("камера не открылась"));
        }

        if (hs != null) {
            try {
                startHighSpeedSession(targets, hs, st);
                fpsRangeText = L.t("скоростной ") + hs.range + " -> " + fps + (relay != null ? L.t(" через GL") : L.t(" через кодер")) + L.t(", камера ") + id;
                return st;
            } catch (Exception e) {
                Log.w(TAG, "high-speed session failed, falling back", e);
                stopLocked();
                Request nr = new Request();
                nr.width = r.width; nr.height = r.height; nr.fps = r.fps; nr.kbps = r.kbps; nr.camera = r.camera;
                nr.highSpeed = false;
                Started fb = start(nr, previewTex, sink);
                fb.note = L.t("скоростной режим не запустился (") + e.getMessage() + L.t("), обычный режим");
                return fb;
            }
        }

        // session
        final CountDownLatch configured = new CountDownLatch(1);
        final boolean[] ok = new boolean[1];
        device.createCaptureSession(targets, new CameraCaptureSession.StateCallback() {
            @Override public void onConfigured(CameraCaptureSession s) { session = s; ok[0] = true; configured.countDown(); }
            @Override public void onConfigureFailed(CameraCaptureSession s) { configured.countDown(); }
        }, camHandler);
        if (!configured.await(5, TimeUnit.SECONDS) || !ok[0]) {
            if (previewSurface != null) {
                // retry without preview, some devices can't do two streams at this size
                targets.remove(previewSurface);
                st.previewSize = null;
                final CountDownLatch c2 = new CountDownLatch(1);
                device.createCaptureSession(targets, new CameraCaptureSession.StateCallback() {
                    @Override public void onConfigured(CameraCaptureSession s) { session = s; ok[0] = true; c2.countDown(); }
                    @Override public void onConfigureFailed(CameraCaptureSession s) { c2.countDown(); }
                }, camHandler);
                c2.await(5, TimeUnit.SECONDS);
            }
            if (!ok[0]) { stopLocked(); throw new Exception(L.t("не удалось настроить камеру")); }
        }

        requestTargets = new ArrayList<Surface>(targets);
        requestRange = choice.range;
        builder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
        for (Surface s : targets) builder.addTarget(s);
        builder.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
        if (hasContinuousAf)
            builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
        Range<Integer> fpsRange = choice.range;
        if (fpsRange != null) builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange);
        fpsRangeText = (fpsRange != null ? fpsRange.getLower() + "-" + fpsRange.getUpper() : "auto")
                + L.t(", камера ") + id + L.t(", макс ") + choice.maxFps;
        builder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
        applyControls();
        return st;
    }

    private List<Surface> requestTargets;
    private Range<Integer> requestRange;

    /** Bare request: targets, auto modes and the fps range, nothing optional. */
    private CaptureRequest.Builder baseRequest() throws CameraAccessException {
        CaptureRequest.Builder b = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
        for (Surface s : requestTargets) b.addTarget(s);
        b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
        if (hasContinuousAf) b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
        b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
        if (requestRange != null) b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, requestRange);
        return b;
    }

    private void startHighSpeedSession(List<Surface> targets, HighSpeedPlan hs, Started st) throws Exception {
        final CountDownLatch configured = new CountDownLatch(1);
        final boolean[] ok = new boolean[1];
        final CameraCaptureSession[] got = new CameraCaptureSession[1];
        CameraCaptureSession.StateCallback cb = new CameraCaptureSession.StateCallback() {
            @Override public void onConfigured(CameraCaptureSession s) { got[0] = s; ok[0] = true; configured.countDown(); }
            @Override public void onConfigureFailed(CameraCaptureSession s) { configured.countDown(); }
        };
        boolean threw = false;
        try {
            device.createConstrainedHighSpeedCaptureSession(targets, cb, camHandler);
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "hs session rejected", e);
            threw = true;
        }
        if (threw || !configured.await(5, TimeUnit.SECONDS) || !ok[0]) {
            if (previewSurface == null) throw new Exception(L.t("сессия не настроилась"));
            // retry with the relay surface only
            targets.remove(previewSurface);
            previewSurface.release();
            previewSurface = null;
            st.previewSize = null;
            final CountDownLatch c2 = new CountDownLatch(1);
            ok[0] = false;
            device.createConstrainedHighSpeedCaptureSession(targets, new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession s) { got[0] = s; ok[0] = true; c2.countDown(); }
                @Override public void onConfigureFailed(CameraCaptureSession s) { c2.countDown(); }
            }, camHandler);
            if (!c2.await(5, TimeUnit.SECONDS) || !ok[0]) throw new Exception(L.t("сессия не настроилась"));
        }
        session = got[0];
        hsSession = (android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession) got[0];
        requestTargets = new ArrayList<Surface>(targets);
        requestRange = hs.range;
        builder = baseRequest();
        applyControls();
    }

    private static Range<Integer> chooseFpsRange(CameraCharacteristics cc, int fps) {
        Range<Integer>[] ranges = cc.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if (ranges == null) return null;
        Range<Integer> best = null;
        // exact upper bound, highest lower bound (e.g. [60,60] beats [15,60]) for a steady frame rate
        for (Range<Integer> rg : ranges) {
            if (rg.getUpper() != fps) continue;
            if (best == null || rg.getLower() > best.getLower()) best = rg;
        }
        if (best != null) return best;
        // otherwise the fastest range not above the request
        for (Range<Integer> rg : ranges) {
            if (rg.getUpper() > fps) continue;
            if (best == null || rg.getUpper() > best.getUpper()
                    || (rg.getUpper().equals(best.getUpper()) && rg.getLower() > best.getLower())) best = rg;
        }
        return best;
    }

    // ---- image controls set from the PC (gear dialog) ----
    private float evCompensation = 0f;   // in EV
    private int wbMode = CaptureRequest.CONTROL_AWB_MODE_AUTO;
    private int edgeMode = -1;           // -1 = camera default
    private int nrMode = -1;
    private boolean aeLock, awbLock;
    volatile String controlsInfo = "";

    private static boolean contains(int[] arr, int v) {
        if (arr == null) return false;
        for (int a : arr) if (a == v) return true;
        return false;
    }

    private void applyControls() {
        try {
            if (builder == null || session == null) return;
            CameraCharacteristics cc = currentChars;
            StringBuilder info = new StringBuilder();
            // Torch: officially not supported in high-speed sessions, but some cameras accept it anyway, so we try.
            // If the camera rejects the request, the torch is the first thing dropped (see dropOrder below).
            builder.set(CaptureRequest.FLASH_MODE, null);
            if (torch && !hasFlash) info.append(L.t("фонарика нет"));
            // zoom: zoom ratio (Android 11+, can switch lenses) or crop region
            Range<Float> zr = null;
            if (Build.VERSION.SDK_INT >= 30 && cc != null) {
                try {
                    Object o = cc.get((CameraCharacteristics.Key<?>) CameraCharacteristics.class
                            .getField("CONTROL_ZOOM_RATIO_RANGE").get(null));
                    if (o instanceof Range) zr = (Range<Float>) o;
                } catch (Throwable ignored) {}
            }
            if (zr != null) {
                float z = Math.max(zr.getLower(), Math.min(zoom * baseZoom, zr.getUpper()));
                try {
                    @SuppressWarnings("unchecked")
                    CaptureRequest.Key<Float> k = (CaptureRequest.Key<Float>) CaptureRequest.class
                            .getField("CONTROL_ZOOM_RATIO").get(null);
                    builder.set(k, z);
                } catch (Throwable t) { zr = null; }
            }
            if (zr == null && activeArray != null) {
                float z = Math.max(1f, Math.min(zoom, maxZoom));
                int cw = (int) (activeArray.width() / z), ch = (int) (activeArray.height() / z);
                int cx = activeArray.centerX(), cy = activeArray.centerY();
                builder.set(CaptureRequest.SCALER_CROP_REGION,
                        new Rect(cx - cw / 2, cy - ch / 2, cx + cw / 2, cy + ch / 2));
            }
            // Optional camera controls. Some cameras (especially in 60 FPS mode) reject some of them; then we drop
            // only the rejected ones, keep everything else (exposure, zoom) working, and report what was dropped.
            List<Object[]> opt = new ArrayList<Object[]>();   // {key, value, name}
            // Torch. Normal mode: always send the state. High-speed mode (picky cameras): leave the key out while
            // the torch is off (that is the state from which "TORCH" is accepted), send TORCH while on, and send
            // OFF only briefly when switching off (without it the camera keeps the torch lit).
            if (hasFlash) {
                Integer fm = null;
                if (hsSession == null) fm = torch ? CaptureRequest.FLASH_MODE_TORCH : CaptureRequest.FLASH_MODE_OFF;
                else if (torch) fm = CaptureRequest.FLASH_MODE_TORCH;
                else if (torchOffPending) fm = CaptureRequest.FLASH_MODE_OFF;
                if (fm != null) opt.add(new Object[]{CaptureRequest.FLASH_MODE, fm, L.t("фонарик")});
            }
            String evText = "";
            if (cc != null) {
                Range<Integer> evr = cc.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE);
                android.util.Rational step = cc.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP);
                if (evr != null && step != null && step.floatValue() > 0 && evr.getUpper() > evr.getLower()) {
                    int idx = Math.round(evCompensation / step.floatValue());
                    idx = Math.max(evr.getLower(), Math.min(evr.getUpper(), idx));
                    opt.add(new Object[]{CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, idx, L.t("экспозиция")});
                    evText = String.format(java.util.Locale.US, "EV %+.1f", idx * step.floatValue());
                } else if (evCompensation != 0) {
                    info.append(info.length() > 0 ? ", " : "").append(L.t("экспозиция не поддерживается"));
                }
                if (contains(cc.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES), wbMode))
                    opt.add(new Object[]{CaptureRequest.CONTROL_AWB_MODE, wbMode, L.t("баланс белого")});
                if (edgeMode >= 0 && contains(cc.get(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES), edgeMode))
                    opt.add(new Object[]{CaptureRequest.EDGE_MODE, edgeMode, L.t("резкость")});
                if (nrMode >= 0 && contains(cc.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES), nrMode))
                    opt.add(new Object[]{CaptureRequest.NOISE_REDUCTION_MODE, nrMode, L.t("шумоподавление")});
                Boolean aeL = cc.get(CameraCharacteristics.CONTROL_AE_LOCK_AVAILABLE);
                if (aeL != null && aeL) opt.add(new Object[]{CaptureRequest.CONTROL_AE_LOCK, aeLock, L.t("фикс. экспозиции")});
                Boolean awbL = cc.get(CameraCharacteristics.CONTROL_AWB_LOCK_AVAILABLE);
                if (awbL != null && awbL) opt.add(new Object[]{CaptureRequest.CONTROL_AWB_LOCK, awbLock, L.t("фикс. баланса")});
            }
            for (Object[] o : opt) setKey(builder, o[0], o[1]);
            List<String> dropped = new ArrayList<String>();
            // drop in this order until the camera accepts the request
            String[] dropOrder = {L.t("фонарик"), L.t("резкость"), L.t("шумоподавление"), L.t("баланс белого"), L.t("фикс. баланса"), L.t("фикс. экспозиции"),
                    L.t("экспозиция"), L.t("зум"), L.t("триггер фокуса"), L.t("всё лишнее")};
            int di = 0;
            String firstError = null;
            while (true) {
                try {
                    submitRepeating();
                    break;
                } catch (Exception e) {
                    if (firstError == null) firstError = e.getMessage();
                    if (di >= dropOrder.length) throw e;
                    String drop = dropOrder[di++];
                    boolean any = false;
                    for (Object[] o : opt) if (o[2].equals(drop)) { setKey(builder, o[0], null); any = true; }
                    if (drop.equals(L.t("зум"))) {
                        builder.set(CaptureRequest.SCALER_CROP_REGION, null);
                        try {
                            setKey(builder, CaptureRequest.class.getField("CONTROL_ZOOM_RATIO").get(null), null);
                        } catch (Throwable ignored) {}
                        any = true;
                    } else if (drop.equals(L.t("триггер фокуса"))) {
                        builder.set(CaptureRequest.CONTROL_AF_TRIGGER, null);
                        any = true;
                    } else if (drop.equals(L.t("всё лишнее"))) {
                        builder = baseRequest();   // keep the stream alive no matter what
                        any = true;
                    }
                    if (any) dropped.add(drop);
                }
            }
            if (dropped.contains(L.t("экспозиция")) || dropped.contains(L.t("всё лишнее"))) setEvEffective(false);
            torchDropped = torch && (dropped.contains(L.t("фонарик")) || dropped.contains(L.t("всё лишнее")));
            dropped.remove(L.t("фонарик"));  // reported by torchStatus() instead
            if (firstError != null) {
                String m = firstError.length() > 90 ? firstError.substring(0, 90) + "…" : firstError;
                info.append(info.length() > 0 ? ", " : "").append(L.t("ошибка камеры: ")).append(m);
            }
            if (!evText.isEmpty() && !dropped.contains(L.t("экспозиция")) && evCompensation != 0)
                info.append(info.length() > 0 ? ", " : "").append(evText);
            if (!dropped.isEmpty())
                info.append(info.length() > 0 ? ", " : "").append(L.t("камера не приняла: ")).append(android.text.TextUtils.join(", ", dropped));
            controlsInfo = info.toString();
        } catch (Exception e) {
            Log.w(TAG, "applyControls", e);
            controlsInfo = L.t("настройки камеры не применились: ") + e.getClass().getSimpleName() + " " + e.getMessage();
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static void setKey(CaptureRequest.Builder b, Object key, Object value) {
        b.set((CaptureRequest.Key) key, value);
    }

    private void submitRepeating() throws CameraAccessException {
        if (hsSession != null)
            hsSession.setRepeatingBurst(hsSession.createHighSpeedRequestList(builder.build()), countFrames, camHandler);
        else
            session.setRepeatingRequest(builder.build(), countFrames, camHandler);
    }

    private boolean torchOffPending;

    boolean isHighSpeed() { return hsSession != null; }
    synchronized void reapplyControls() { applyControls(); }
    boolean isTorchRequested() { return torch; }
    boolean isTorchLit() { return flashState == CaptureResult.FLASH_STATE_FIRED; }

    // torch check in 60 FPS mode, counted in camera frames processed since the torch was requested
    private volatile int torchFrames;
    private volatile boolean torchSeenLit, torchStateReported;
    int torchFramesSinceRequest() { return torchFrames; }
    boolean torchConfirmed() { return torchSeenLit; }
    boolean torchStateReported() { return torchStateReported; }
    boolean torchRejected() { return torchDropped; }

    synchronized void setTorch(boolean on) {
        boolean wasOn = torch;
        if (on && !wasOn) { torchFrames = 0; torchSeenLit = false; torchStateReported = false; }
        torch = on;
        torchOffPending = !on && wasOn && hsSession != null;
        applyControls();
        Handler h = camHandler;
        // Re-assert the torch state twice: right after the session starts some cameras (1080p/120) miss the first
        // torch command. Re-sending the same request is harmless if it already worked.
        if (h != null && hsSession != null) {
            final boolean want = on;
            for (long delay : new long[]{400, 1200}) {
                h.postDelayed(new Runnable() {
                    public void run() {
                        synchronized (CameraStreamer.this) {
                            if (torch == want && session != null) applyControls();
                        }
                    }
                }, delay);
            }
        }
        if (torchOffPending && h != null) {
            h.postDelayed(new Runnable() {
                public void run() {
                    synchronized (CameraStreamer.this) {
                        if (!torchOffPending || torch) return;
                        torchOffPending = false;
                        applyControls();
                    }
                }
            }, 1600);
        }
    }

    synchronized void setZoom(float z) { zoom = z; applyControls(); }

    private volatile int flashState = -1;
    private volatile boolean torchDropped;

    /** Live torch status for the stats line ("" when the torch is off). */
    String torchStatus() {
        if (!torch || !hasFlash) return "";
        if (torchDropped) return L.t("фонарик: камера отклонила команду") + (hsSession != null ? L.t(" в режиме 60 FPS") : "");
        return L.t("фонарик включён");
    }

    // ---- does the camera really change its exposure when we ask? (some ignore it, e.g. in 60 FPS mode) ----
    private volatile double exposureProduct;      // exposure time x ISO of the latest frame
    volatile boolean evEffective = true;
    volatile Runnable onEvEffectiveChanged;

    private void setEvEffective(boolean v) {
        if (evEffective == v) return;
        evEffective = v;
        Runnable r = onEvEffectiveChanged;
        if (r != null) r.run();
    }

    synchronized void setExposure(float ev) {
        final float before = evCompensation;
        evCompensation = ev;
        applyControls();
        final double prodBefore = exposureProduct;
        final float delta = ev - before;
        Handler h = camHandler;
        if (h == null || Math.abs(delta) < 0.3f || prodBefore <= 0 || aeLock) return;
        h.postDelayed(new Runnable() {
            public void run() {
                double after = exposureProduct;
                if (after <= 0) return;
                double measured = Math.log(after / prodBefore) / Math.log(2);
                // camera moved less than a third of the requested EV change -> it ignores compensation
                boolean works = Math.signum(measured) == Math.signum(delta) && Math.abs(measured) >= Math.abs(delta) * 0.33;
                Log.i(TAG, "EV check: asked " + delta + " got " + measured);
                setEvEffective(works);
            }
        }, 1000);
    }
    synchronized void setWhiteBalance(int mode) { wbMode = mode; applyControls(); }
    synchronized void setEdge(int mode) { edgeMode = mode; applyControls(); }
    synchronized void setNoiseReduction(int mode) { nrMode = mode; applyControls(); }
    synchronized void setAeLock(boolean on) { aeLock = on; applyControls(); }
    synchronized void setAwbLock(boolean on) { awbLock = on; applyControls(); }

    private boolean hasAutoAf;
    private volatile int afState = -1;
    private volatile String focusInfo = "";
    private volatile long focusInfoUntil;
    private Runnable afRestore;

    /**
     * "Focus" button. In continuous-video AF a trigger only LOCKS the current focus (looks like nothing happens),
     * so do it like tap-to-focus: one-shot AF on the centre of the frame, then back to continuous after 3 s.
     */
    synchronized void triggerAutofocus() {
        try {
            if (builder == null || session == null) return;
            if (!hasAutoAf) {
                focusInfo = L.t("у этой камеры фиксированный фокус");
                focusInfoUntil = System.currentTimeMillis() + 4000;
                return;
            }
            CameraCharacteristics cc = currentChars;
            builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO);
            Integer maxAf = cc != null ? cc.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) : null;
            if (maxAf != null && maxAf > 0 && activeArray != null) {
                int w = activeArray.width() / 3, h = activeArray.height() / 3;
                int cx = activeArray.centerX(), cy = activeArray.centerY();
                builder.set(CaptureRequest.CONTROL_AF_REGIONS, new android.hardware.camera2.params.MeteringRectangle[]{
                        new android.hardware.camera2.params.MeteringRectangle(cx - w / 2, cy - h / 2, w, h,
                                android.hardware.camera2.params.MeteringRectangle.METERING_WEIGHT_MAX - 1)});
            }
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE);
            submitRepeating();
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START);
            if (hsSession != null) hsSession.captureBurst(hsSession.createHighSpeedRequestList(builder.build()), countFrames, camHandler);
            else session.capture(builder.build(), countFrames, camHandler);
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE);
            focusInfo = L.t("фокусируюсь…");
            focusInfoUntil = System.currentTimeMillis() + 4000;
            Handler h = camHandler;
            if (h != null) {
                if (afRestore != null) h.removeCallbacks(afRestore);
                afRestore = new Runnable() {
                    public void run() { restoreContinuousAf(); }
                };
                h.postDelayed(afRestore, 3000);
            }
        } catch (Exception e) {
            Log.w(TAG, "af", e);
            focusInfo = L.t("камера не дала сфокусироваться");
            focusInfoUntil = System.currentTimeMillis() + 4000;
        }
    }

    private synchronized void restoreContinuousAf() {
        try {
            if (builder == null || session == null) return;
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_CANCEL);
            if (hsSession != null) hsSession.captureBurst(hsSession.createHighSpeedRequestList(builder.build()), null, camHandler);
            else session.capture(builder.build(), null, camHandler);
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, null);
            builder.set(CaptureRequest.CONTROL_AF_REGIONS, null);
            if (hasContinuousAf) builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
            applyControls();
        } catch (Exception e) {
            Log.w(TAG, "af restore", e);
        }
    }

    /** Short-lived focus status for the stats line. */
    String focusStatus() {
        if (System.currentTimeMillis() > focusInfoUntil) return "";
        if (focusInfo.startsWith(L.t("фокусируюсь"))) {
            if (afState == CaptureResult.CONTROL_AF_STATE_FOCUSED_LOCKED) return L.t("фокус наведён");
            if (afState == CaptureResult.CONTROL_AF_STATE_NOT_FOCUSED_LOCKED) return L.t("фокус: не удалось навести");
        }
        return focusInfo;
    }

    void setBitrate(int kbps) {
        MediaCodec e = encoder;
        if (e == null) return;
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, kbps * 1000);
            e.setParameters(b);
        } catch (Exception ex) {
            Log.w(TAG, "bitrate", ex);
        }
    }

    void requestKeyFrame() {
        MediaCodec e = encoder;
        if (e == null) return;
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            e.setParameters(b);
        } catch (Exception ex) {
            Log.w(TAG, "keyframe", ex);
        }
    }

    private void fail(String msg) {
        Sink s = sink;
        if (s != null) s.onStreamError(msg);
    }

    private void startDrain() {
        final MediaCodec enc = encoder;
        drainThread = new Thread(new Runnable() {
            @Override public void run() {
                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                try {
                    while (running) {
                        int idx = enc.dequeueOutputBuffer(info, 100000);
                        if (idx < 0) continue;
                        ByteBuffer buf = enc.getOutputBuffer(idx);
                        if (buf != null && info.size > 0) {
                            byte[] data = new byte[info.size];
                            buf.position(info.offset);
                            buf.limit(info.offset + info.size);
                            buf.get(data);
                            int type;
                            if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) type = Protocol.PKT_CONFIG;
                            else if ((info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0) type = Protocol.PKT_KEY;
                            else type = Protocol.PKT_FRAME;
                            if (type != Protocol.PKT_CONFIG) {
                                encodedFrames.incrementAndGet();
                                // Self-check the clock base used for latency measurement: frame timestamps
                                // must be close to "now"; if not, the camera uses the other clock.
                                if (!clockChecked) {
                                    clockChecked = true;
                                    if (Math.abs(nowUs() - info.presentationTimeUs) > 2_000_000L)
                                        realtimeTimestamps = !realtimeTimestamps;
                                }
                            }
                            Sink s = sink;
                            if (s != null) s.onPacket(type, info.presentationTimeUs, data);
                        }
                        enc.releaseOutputBuffer(idx, false);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
                    }
                } catch (Exception e) {
                    if (running) {
                        Log.w(TAG, "drain", e);
                        fail(L.t("ошибка кодировщика"));
                    }
                }
            }
        }, "encoder-drain");
        drainThread.start();
    }

    synchronized void stop() { stopLocked(); }

    private void stopLocked() {
        sink = null;
        running = false;
        try { if (session != null) session.close(); } catch (Exception ignored) {}
        session = null;
        hsSession = null;
        builder = null;
        try { if (device != null) device.close(); } catch (Exception ignored) {}
        device = null;
        if (drainThread != null) {
            try { drainThread.join(1000); } catch (InterruptedException ignored) {}
            drainThread = null;
        }
        if (relay != null) { relay.release(); relay = null; }
        if (encoder != null) {
            try { encoder.stop(); } catch (Exception ignored) {}
            try { encoder.release(); } catch (Exception ignored) {}
            encoder = null;
        }
        if (encoderSurface != null) { encoderSurface.release(); encoderSurface = null; }
        if (previewSurface != null) { previewSurface.release(); previewSurface = null; }
        if (camThread != null) { camThread.quitSafely(); camThread = null; camHandler = null; }
    }
}
