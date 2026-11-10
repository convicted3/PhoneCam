package com.phonecam.app;

import android.content.Context;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.MediaCodec;
import android.os.Build;
import android.util.Range;
import android.util.Size;

import java.util.ArrayList;
import java.util.List;
import java.util.Set;

/** Human readable dump of what every camera reports to third-party apps (for diagnosing fps limits). */
final class CameraReport {
    private CameraReport() {}

    static String build(Context ctx) {
        CameraManager m = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
        StringBuilder sb = new StringBuilder();
        sb.append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(", Android ")
                .append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n\n");
        List<String> ids = new ArrayList<String>();
        try { for (String id : m.getCameraIdList()) ids.add(id); } catch (Exception ignored) {}
        int listed = ids.size();
        for (int i = 0; i < 16; i++) {
            String id = String.valueOf(i);
            if (ids.contains(id)) continue;
            try { m.getCameraCharacteristics(id); ids.add(id); } catch (Throwable ignored) {}
        }
        for (int i = 0; i < ids.size(); i++) {
            String id = ids.get(i);
            try {
                describe(sb, m, id, i >= listed ? " (скрытая)" : "");
                if (Build.VERSION.SDK_INT >= 28) {
                    Set<String> phys = m.getCameraCharacteristics(id).getPhysicalCameraIds();
                    if (phys != null && !phys.isEmpty()) {
                        sb.append("  физические камеры: ").append(phys).append('\n');
                        for (String p : phys) if (!ids.contains(p)) describe(sb, m, p, " (физическая в " + id + ")");
                    }
                }
            } catch (Throwable t) {
                sb.append("Камера ").append(id).append(": ошибка ").append(t).append('\n');
            }
            sb.append('\n');
        }
        return sb.toString();
    }

    private static void describe(StringBuilder sb, CameraManager m, String id, String note) throws Exception {
        CameraCharacteristics cc = m.getCameraCharacteristics(id);
        Integer f = cc.get(CameraCharacteristics.LENS_FACING);
        String facing = f == null ? "?" : f == CameraCharacteristics.LENS_FACING_BACK ? "задняя"
                : f == CameraCharacteristics.LENS_FACING_FRONT ? "фронтальная" : "внешняя";
        Integer lvl = cc.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL);
        float[] focal = cc.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
        sb.append("Камера ").append(id).append(note).append(": ").append(facing)
                .append(", уровень ").append(level(lvl));
        if (focal != null && focal.length > 0) sb.append(", фокус ").append(focal[0]).append("мм");
        sb.append('\n');

        Range<Integer>[] ranges = cc.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        sb.append("  fps-диапазоны: ");
        if (ranges != null) for (Range<Integer> r : ranges) sb.append(r).append(' ');
        sb.append('\n');

        StreamConfigurationMap map = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if (map != null) {
            for (Size s : new Size[]{new Size(1920, 1080), new Size(1280, 720), new Size(3840, 2160)}) {
                long d = 0;
                try { d = map.getOutputMinFrameDuration(MediaCodec.class, s); } catch (Exception ignored) {}
                sb.append("  ").append(s).append(": ")
                        .append(d > 0 ? "макс " + Math.round(1e9 / d) + " к/с" : "нет").append('\n');
            }
            int[] caps = cc.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
            boolean hs = false;
            if (caps != null) for (int c : caps)
                if (c == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO) hs = true;
            if (hs) {
                sb.append("  скоростная съёмка:");
                for (Size s : map.getHighSpeedVideoSizes()) {
                    sb.append(' ').append(s).append('[');
                    for (Range<Integer> r : map.getHighSpeedVideoFpsRangesFor(s)) sb.append(r);
                    sb.append(']');
                }
                sb.append('\n');
            }
        }
    }

    private static String level(Integer l) {
        if (l == null) return "?";
        switch (l) {
            case CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY: return "LEGACY";
            case CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED: return "LIMITED";
            case CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_FULL: return "FULL";
            case 3: return "LEVEL_3";
            case 4: return "EXTERNAL";
            default: return String.valueOf(l);
        }
    }
}
