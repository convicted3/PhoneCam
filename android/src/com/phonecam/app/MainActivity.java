package com.phonecam.app;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.RectF;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.hardware.display.DisplayManager;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Size;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;

public class MainActivity extends Activity implements StreamServer.Host {

    private static final int REQ_CAMERA = 1;

    private Handler ui;
    private CameraStreamer streamer;
    private StreamServer server;
    private TextureView preview;
    private FrameLayout previewBox;
    private TextView ipText, statusText, hintText;
    private Button dimButton;
    private boolean dimmed;
    private WifiManager.WifiLock wifiLock;
    private WifiManager.MulticastLock multicastLock;
    private volatile CameraStreamer.Started active;
    private volatile Size previewSize;
    private DisplayManager.DisplayListener displayListener;
    private int lastRotation = -1;

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                | WindowManager.LayoutParams.FLAG_FULLSCREEN);
        ui = new Handler(Looper.getMainLooper());
        streamer = new CameraStreamer(this);
        server = new StreamServer(this);
        // The screen follows the phone's physical position; the video orientation is a separate setting.
        portraitMode = getSharedPreferences("phonecam", MODE_PRIVATE).getBoolean("portrait", false);
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR);
        buildUi();

        displayListener = new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int id) {}
            @Override public void onDisplayRemoved(int id) {}
            @Override public void onDisplayChanged(int id) { onRotationMaybeChanged(); }
        };
    }

    private LinearLayout root;
    private android.widget.ScrollView infoScroll;
    private Button orientButton;
    private Button torchButton;
    private android.widget.SeekBar zoomBar;
    private TextView zoomText;
    private boolean torchUi;

    // ---- screen light for the front camera ----
    private FrameLayout screenLight;

    @Override
    public void setScreenLight(final boolean on) {
        ui.post(new Runnable() { public void run() { showScreenLight(on); } });
    }

    private void showScreenLight(boolean on) {
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        if (on) {
            if (screenLight == null) {
                screenLight = new FrameLayout(this);
                screenLight.setBackgroundColor(Color.WHITE);
                TextView t = new TextView(this);
                t.setText(L.t("Подсветка экраном\nкоснись, чтобы выключить"));
                t.setTextColor(Color.rgb(170, 170, 170));
                t.setTextSize(13);
                t.setGravity(Gravity.CENTER);
                screenLight.addView(t, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
                screenLight.setOnClickListener(new View.OnClickListener() {
                    @Override public void onClick(View v) {
                        showScreenLight(false);
                        setTorchUi(false);
                        server.localTorch(false);  // tells the PC too
                    }
                });
            }
            if (screenLight.getParent() == null)
                addContentView(screenLight, new FrameLayout.LayoutParams(-1, -1));
            screenLight.setVisibility(View.VISIBLE);
            screenLight.bringToFront();
            lp.screenBrightness = 1f;
        } else {
            if (screenLight != null) screenLight.setVisibility(View.GONE);
            lp.screenBrightness = dimmed ? 0.01f : WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
        }
        getWindow().setAttributes(lp);
    }

    private void setTorchUi(boolean on) {
        torchUi = on;
        String name = server != null && server.frontCamera() ? L.t("Подсветка") : L.t("Фонарик");
        torchButton.setText(name + (on ? L.t(": вкл") : L.t(": выкл")));
        GradientDrawable d = new GradientDrawable();
        d.setColor(on ? Color.rgb(76, 201, 140) : Color.rgb(40, 44, 56));
        d.setCornerRadius(dp(10));
        torchButton.setBackground(d);
        torchButton.setTextColor(on ? Color.rgb(10, 22, 16) : Color.WHITE);
    }

    @Override
    public void onRemoteControl(final boolean torch, final float zoom) {
        ui.post(new Runnable() {
            public void run() {
                setTorchUi(torch);
                zoomBar.setProgress(Math.round((zoom - 1f) * 10));
            }
        });
    }
    private boolean portraitMode;

    private Button flatButton(String text) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(13);
        b.setMaxLines(1);
        b.setEllipsize(android.text.TextUtils.TruncateAt.END);
        GradientDrawable d = new GradientDrawable();
        d.setColor(Color.rgb(40, 44, 56));
        d.setCornerRadius(dp(10));
        b.setBackground(d);
        b.setPadding(dp(8), 0, dp(8), 0);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        return b;
    }

    private void buildUi() {
        int bg = Color.rgb(16, 18, 24);
        int accent = Color.rgb(76, 201, 140);

        root = new LinearLayout(this);
        root.setBackgroundColor(bg);
        root.setPadding(dp(16), dp(16), dp(16), dp(16));

        // ---- preview (first child: top in portrait, left in landscape) ----
        previewBox = new FrameLayout(this);
        GradientDrawable pb = new GradientDrawable();
        pb.setColor(Color.BLACK);
        pb.setCornerRadius(dp(12));
        previewBox.setBackground(pb);
        root.addView(previewBox);

        TextView noPrev = new TextView(this);
        noPrev.setText(L.t("Превью появится после подключения"));
        noPrev.setTextColor(Color.rgb(110, 116, 130));
        noPrev.setGravity(Gravity.CENTER);
        previewBox.addView(noPrev, new FrameLayout.LayoutParams(-1, -1));

        preview = new TextureView(this);
        previewBox.addView(preview, new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER));
        preview.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override public void onSurfaceTextureAvailable(SurfaceTexture s, int w, int h) { configureTransform(); }
            @Override public void onSurfaceTextureSizeChanged(SurfaceTexture s, int w, int h) { configureTransform(); }
            @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture s) { return true; }
            @Override public void onSurfaceTextureUpdated(SurfaceTexture s) {}
        });
        previewBox.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { if (dimmed) setDimmed(false); }
        });

        // ---- info column ----
        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        infoScroll = new android.widget.ScrollView(this);
        infoScroll.setFillViewport(true);
        infoScroll.addView(info, new android.widget.FrameLayout.LayoutParams(-1, -2));
        root.addView(infoScroll);

        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        android.widget.ImageView logo = new android.widget.ImageView(this);
        logo.setImageDrawable(getApplicationInfo().loadIcon(getPackageManager()));
        head.addView(logo, new LinearLayout.LayoutParams(dp(40), dp(40)));
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        TextView title = new TextView(this);
        android.text.SpannableString name = new android.text.SpannableString("PhoneCam");
        name.setSpan(new android.text.style.ForegroundColorSpan(Color.WHITE), 0, 5, 0);
        name.setSpan(new android.text.style.ForegroundColorSpan(accent), 5, 8, 0);
        title.setText(name);
        title.setTextSize(24);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        titles.addView(title);
        TextView sub = new TextView(this);
        sub.setText(L.t("телефон как вебкамера"));
        sub.setTextColor(Color.rgb(150, 156, 170));
        sub.setTextSize(13);
        titles.addView(sub);
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(-2, -2);
        tlp.leftMargin = dp(12);
        head.addView(titles, tlp);
        LinearLayout.LayoutParams hlp0 = new LinearLayout.LayoutParams(-1, -2);
        info.addView(head, hlp0);

        TextView ipLabel = label(L.t("IP-адрес для подключения"));
        ((LinearLayout.LayoutParams) ipLabel.getLayoutParams()).topMargin = dp(16);
        info.addView(ipLabel, ipLabel.getLayoutParams());

        ipText = new TextView(this);
        ipText.setTextColor(accent);
        ipText.setTextSize(24);
        ipText.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        info.addView(ipText);

        TextView portText = new TextView(this);
        portText.setText(L.t("Порт ") + Protocol.TCP_PORT + L.t("  •  USB: включи «Отладку по USB»"));
        portText.setTextColor(Color.rgb(150, 156, 170));
        portText.setTextSize(13);
        info.addView(portText);

        TextView stLabel = label(L.t("Статус"));
        ((LinearLayout.LayoutParams) stLabel.getLayoutParams()).topMargin = dp(16);
        info.addView(stLabel, stLabel.getLayoutParams());

        statusText = new TextView(this);
        statusText.setTextColor(Color.WHITE);
        statusText.setTextSize(15);
        statusText.setText(L.t("Запуск…"));
        info.addView(statusText);

        // three compact buttons in one row: always fully visible in both orientations
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        orientButton = flatButton("");
        dimButton = flatButton(L.t("Затемнить"));
        Button camsButton = flatButton(L.t("Инфо камер"));
        Button[] bs = {orientButton, dimButton, camsButton};
        for (int k = 0; k < bs.length; k++) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(44), 1f);
            if (k > 0) lp.leftMargin = dp(8);
            row.addView(bs[k], lp);
        }
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(-1, -2);
        rlp.topMargin = dp(14);
        info.addView(row, rlp);
        orientButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { setPortraitMode(!portraitMode); }
        });
        dimButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { setDimmed(!dimmed); }
        });
        camsButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { showCameraReport(); }
        });

        // ---- torch + zoom (work while streaming, kept in sync with the PC) ----
        LinearLayout row2 = new LinearLayout(this);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        row2.setGravity(Gravity.CENTER_VERTICAL);
        torchButton = flatButton(L.t("Фонарик: выкл"));
        row2.addView(torchButton, new LinearLayout.LayoutParams(dp(130), dp(44)));
        TextView zl = new TextView(this);
        zl.setText(L.t("Зум"));
        zl.setTextColor(Color.rgb(150, 156, 170));
        zl.setTextSize(13);
        LinearLayout.LayoutParams zlp = new LinearLayout.LayoutParams(-2, -2);
        zlp.leftMargin = dp(14);
        row2.addView(zl, zlp);
        zoomBar = new android.widget.SeekBar(this);
        zoomBar.setMax(40);  // 1.0x .. 5.0x
        row2.addView(zoomBar, new LinearLayout.LayoutParams(0, dp(44), 1f));
        zoomText = new TextView(this);
        zoomText.setText("1.0×");
        zoomText.setTextColor(Color.WHITE);
        zoomText.setTextSize(13);
        row2.addView(zoomText, new LinearLayout.LayoutParams(dp(44), -2));
        LinearLayout.LayoutParams r2lp = new LinearLayout.LayoutParams(-1, -2);
        r2lp.topMargin = dp(10);
        info.addView(row2, r2lp);
        TextView tzHint = new TextView(this);
        tzHint.setText(L.t("Если фонарик или зум не отвечают - переподключись к ПК."));
        tzHint.setTextColor(Color.rgb(150, 156, 170));
        tzHint.setTextSize(12);
        LinearLayout.LayoutParams tzlp = new LinearLayout.LayoutParams(-1, -2);
        tzlp.topMargin = dp(4);
        info.addView(tzHint, tzlp);
        torchButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (!server.streaming()) { statusText.setText(L.t("Фонарик и зум работают во время трансляции")); return; }
                setTorchUi(!torchUi);
                server.localTorch(torchUi);
            }
        });
        zoomBar.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(android.widget.SeekBar b, int p, boolean fromUser) {
                float z = 1f + p / 10f;
                zoomText.setText(String.format(java.util.Locale.US, "%.1f×", z));
                if (fromUser && server.streaming()) server.localZoom(z);
            }
            @Override public void onStartTrackingTouch(android.widget.SeekBar b) {}
            @Override public void onStopTrackingTouch(android.widget.SeekBar b) {}
        });

        hintText = new TextView(this);
        hintText.setTextColor(Color.rgb(150, 156, 170));
        hintText.setTextSize(12);
        hintText.setText(L.t("Не сворачивай приложение во время трансляции. Экран можно затемнить. "
                + "Кнопка «Видео» переключает ориентацию картинки на ПК (горизонтальная или вертикальная), экран приложения поворачивается сам."));
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(-1, -2);
        hlp.topMargin = dp(12);
        info.addView(hintText, hlp);

        setContentView(root);
        updateOrientButton();
        applyOrientationLayout();
    }

    private boolean isPortraitScreen() {
        return getResources().getConfiguration().orientation == android.content.res.Configuration.ORIENTATION_PORTRAIT;
    }

    /** Preview on top in portrait, on the left in landscape. */
    private void applyOrientationLayout() {
        if (root == null) return;
        boolean portrait = isPortraitScreen();
        root.setOrientation(portrait ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams plp, ilp;
        if (portrait) {
            plp = new LinearLayout.LayoutParams(-1, 0, 1f);
            ilp = new LinearLayout.LayoutParams(-1, 0, 1.1f);
            ilp.topMargin = dp(14);
        } else {
            plp = new LinearLayout.LayoutParams(0, -1, 1.3f);
            ilp = new LinearLayout.LayoutParams(0, -1, 1f);
            ilp.leftMargin = dp(16);
        }
        previewBox.setLayoutParams(plp);
        infoScroll.setLayoutParams(ilp);
        previewBox.post(new Runnable() { public void run() { layoutPreview(); configureTransform(); } });
    }

    @Override
    public void onConfigurationChanged(android.content.res.Configuration c) {
        super.onConfigurationChanged(c);
        applyOrientationLayout();
        onRotationMaybeChanged();
    }

    private void updateOrientButton() {
        if (orientButton != null) orientButton.setText(portraitMode ? L.t("Видео: вертик.") : L.t("Видео: горизонт."));
    }

    /** Switch ONLY the video orientation sent to the PC (the screen keeps following the phone). */
    void setPortraitMode(boolean portrait) {
        portraitMode = portrait;
        getSharedPreferences("phonecam", MODE_PRIVATE).edit().putBoolean("portrait", portrait).apply();
        updateOrientButton();
        CameraStreamer.Started st = active;
        if (st != null) server.notifyRotation(currentRotation(st));
    }

    @Override
    public void reapplyCameraSettings() {
        // exposure, white balance, etc. live in CameraStreamer fields and survive a restart; push them again
        streamer.reapplyControls();
    }

    @Override
    public String batteryInfo() {
        try {
            android.content.Intent i = getApplicationContext().registerReceiver(null,
                    new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED));
            if (i == null) return null;
            int level = i.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1);
            int scale = i.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, 100);
            if (level < 0 || scale <= 0) return null;
            int st = i.getIntExtra(android.os.BatteryManager.EXTRA_STATUS, -1);
            boolean charging = st == android.os.BatteryManager.BATTERY_STATUS_CHARGING
                    || st == android.os.BatteryManager.BATTERY_STATUS_FULL;
            return "battery " + Math.round(level * 100f / scale) + " " + (charging ? 1 : 0);
        } catch (Exception e) {
            return null;
        }
    }

    @Override
    public void requestOrientation(final boolean portrait) {
        ui.post(new Runnable() { public void run() { setPortraitMode(portrait); } });
    }

    private void showCameraReport() {
        final String report = CameraReport.build(this);
        android.widget.ScrollView sv = new android.widget.ScrollView(this);
        TextView tv = new TextView(this);
        tv.setText(report);
        tv.setTextSize(12);
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setTextIsSelectable(true);
        tv.setPadding(dp(16), dp(12), dp(16), dp(12));
        sv.addView(tv);
        new android.app.AlertDialog.Builder(this)
                .setTitle(L.t("Что камеры сообщают приложениям"))
                .setView(sv)
                .setPositiveButton(L.t("Скопировать"), new android.content.DialogInterface.OnClickListener() {
                    @Override public void onClick(android.content.DialogInterface d, int w) {
                        android.content.ClipboardManager cm =
                                (android.content.ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                        if (cm != null) cm.setPrimaryClip(android.content.ClipData.newPlainText("PhoneCam", report));
                        android.widget.Toast.makeText(MainActivity.this, L.t("Скопировано"), android.widget.Toast.LENGTH_SHORT).show();
                    }
                })
                .setNegativeButton(L.t("Закрыть"), null)
                .show();
    }

    private TextView label(String s) {
        TextView t = new TextView(this);
        t.setText(s.toUpperCase());
        t.setTextColor(Color.rgb(110, 116, 130));
        t.setTextSize(11);
        t.setLetterSpacing(0.08f);
        t.setLayoutParams(new LinearLayout.LayoutParams(-2, -2));
        return t;
    }

    private void setDimmed(boolean d) {
        dimmed = d;
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.screenBrightness = d ? 0.01f : WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
        getWindow().setAttributes(lp);
        dimButton.setText(d ? L.t("Вернуть яркость") : L.t("Затемнить экран"));
    }

    @Override
    protected void onStart() {
        super.onStart();
        refreshIps();
        if (Build.VERSION.SDK_INT >= 23
                && checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            statusText.setText(L.t("Нужно разрешение на камеру"));
            requestPermissions(new String[]{Manifest.permission.CAMERA}, REQ_CAMERA);
            return;
        }
        startServer();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        if (requestCode == REQ_CAMERA) {
            if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) startServer();
            else statusText.setText(L.t("Без разрешения на камеру приложение не работает"));
        }
    }

    private void startServer() {
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wm != null) {
            try {
                if (wifiLock == null) {
                    // API 29+: LOW_LATENCY (4) turns off Wi-Fi power save, which otherwise batches packets.
                    int mode = Build.VERSION.SDK_INT >= 29 ? 4 : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
                    wifiLock = wm.createWifiLock(mode, "phonecam");
                    wifiLock.setReferenceCounted(false);
                }
                wifiLock.acquire();
                if (multicastLock == null) {
                    multicastLock = wm.createMulticastLock("phonecam");
                    multicastLock.setReferenceCounted(false);
                }
                multicastLock.acquire();
            } catch (Exception ignored) {}
        }
        DisplayManager dm = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        if (dm != null) dm.registerDisplayListener(displayListener, ui);
        server.start();
    }

    @Override
    protected void onStop() {
        super.onStop();
        server.stop();
        streamer.stop();
        DisplayManager dm = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        if (dm != null) dm.unregisterDisplayListener(displayListener);
        try { if (wifiLock != null) wifiLock.release(); } catch (Exception ignored) {}
        try { if (multicastLock != null) multicastLock.release(); } catch (Exception ignored) {}
        server = new StreamServer(this);
    }

    private void refreshIps() {
        StringBuilder sb = new StringBuilder();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                String n = ni.getName();
                if (n.startsWith("rmnet") || n.startsWith("ccmni") || n.startsWith("dummy") || n.startsWith("tun"))
                    continue;
                for (InetAddress a : Collections.list(ni.getInetAddresses())) {
                    if (a instanceof Inet4Address) {
                        if (sb.length() > 0) sb.append('\n');
                        sb.append(a.getHostAddress());
                    }
                }
            }
        } catch (Exception ignored) {}
        ipText.setText(sb.length() > 0 ? sb.toString() : L.t("нет Wi-Fi (можно по USB)"));
    }

    // ---- StreamServer.Host ----

    @Override
    public CameraStreamer.Started startStream(CameraStreamer.Request r, CameraStreamer.Sink sink) throws Exception {
        SurfaceTexture tex = preview.isAvailable() ? preview.getSurfaceTexture() : null;
        CameraStreamer.Started st = streamer.start(r, tex, sink);
        active = st;
        previewSize = st.previewSize;
        ui.post(new Runnable() { public void run() { layoutPreview(); configureTransform(); } });
        return st;
    }

    @Override
    public void stopStream() {
        active = null;
        streamer.stop();
    }

    @Override
    public CameraStreamer streamer() { return streamer; }

    /** Degrees the PC must rotate the picture by so it's upright. */
    @Override
    public int currentRotation(CameraStreamer.Started st) {
        // Use the screen rotation only to tell which way up the phone is held, within the chosen video mode:
        // landscape video -> 90 or 270, portrait video -> 0 or 180.
        int disp = displayRotationDegrees();
        int rot;
        if (portraitMode) rot = (disp == 180) ? 180 : 0;
        else rot = (disp == 270) ? 270 : 90;
        int r = st.front ? (st.sensorOrientation + rot) % 360 : (st.sensorOrientation - rot + 360) % 360;
        // clockwise rotation the PC applies: 0/180 in landscape, 90/270 in portrait
        return r;
    }

    private int displayRotationDegrees() {
        switch (getWindowManager().getDefaultDisplay().getRotation()) {
            case Surface.ROTATION_90: return 90;
            case Surface.ROTATION_180: return 180;
            case Surface.ROTATION_270: return 270;
            default: return 0;
        }
    }

    private void onRotationMaybeChanged() {
        int r = displayRotationDegrees();
        if (r == lastRotation) return;
        lastRotation = r;
        configureTransform();
        CameraStreamer.Started st = active;
        if (st != null) server.notifyRotation(currentRotation(st));
    }

    @Override
    public void onStatus(final String text, final boolean streaming) {
        ui.post(new Runnable() {
            public void run() {
                statusText.setText(text);
                if (streaming && torchButton != null) setTorchUi(torchUi);
                statusText.setTextColor(streaming ? Color.rgb(76, 201, 140) : Color.WHITE);
                if (!streaming) {
                    previewSize = null;
                    if (torchButton != null) { setTorchUi(false); zoomBar.setProgress(0); }
                    showScreenLight(false);
                    refreshIps();
                }
            }
        });
    }

    // ---- preview geometry ----

    private void layoutPreview() {
        Size ps = previewSize;
        int bw = previewBox.getWidth(), bh = previewBox.getHeight();
        if (ps == null || bw == 0 || bh == 0) return;
        // the camera buffer is landscape; on a portrait screen it is shown rotated
        float ar = isPortraitScreen() ? (float) ps.getHeight() / ps.getWidth() : (float) ps.getWidth() / ps.getHeight();
        int w = bw, h = (int) (bw / ar);
        if (h > bh) { h = bh; w = (int) (bh * ar); }
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(w, h, Gravity.CENTER);
        preview.setLayoutParams(lp);
    }

    private void configureTransform() {
        Size ps = previewSize;
        int vw = preview.getWidth(), vh = preview.getHeight();
        if (ps == null || vw == 0 || vh == 0) return;
        int rotation = getWindowManager().getDefaultDisplay().getRotation();
        Matrix m = new Matrix();
        RectF viewRect = new RectF(0, 0, vw, vh);
        RectF bufferRect = new RectF(0, 0, ps.getHeight(), ps.getWidth());
        float cx = viewRect.centerX(), cy = viewRect.centerY();
        if (rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270) {
            bufferRect.offset(cx - bufferRect.centerX(), cy - bufferRect.centerY());
            m.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL);
            float scale = Math.max((float) vh / ps.getHeight(), (float) vw / ps.getWidth());
            m.postScale(scale, scale, cx, cy);
            m.postRotate(90 * (rotation - 2), cx, cy);
        } else if (rotation == Surface.ROTATION_180) {
            m.postRotate(180, cx, cy);
        }
        preview.setTransform(m);
    }
}
