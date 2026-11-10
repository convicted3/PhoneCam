package com.phonecam.app;

import android.os.Build;
import android.util.Log;

import java.io.BufferedOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.concurrent.LinkedBlockingDeque;
import java.util.concurrent.TimeUnit;

/** TCP server (one client at a time) + UDP discovery responder. */
final class StreamServer {
    private static final String TAG = "PhoneCam";

    interface Host {
        /** Called on a worker thread. Must start camera and return result, or throw. */
        CameraStreamer.Started startStream(CameraStreamer.Request r, CameraStreamer.Sink sink) throws Exception;
        void stopStream();
        CameraStreamer streamer();
        int currentRotation(CameraStreamer.Started st);
        void onStatus(String text, boolean streaming);
        /** Front camera: the phone screen lights the face instead of the (missing) torch. */
        void setScreenLight(boolean on);
        /** The PC changed torch / zoom: update the phone's controls (no echo back). */
        void onRemoteControl(boolean torch, float zoom);
        /** Re-apply exposure / white balance etc. after the camera was restarted. */
        void reapplyCameraSettings();
        /** PC asked to switch portrait / landscape. */
        void requestOrientation(boolean portrait);
        /** "battery <percent> <charging 0|1>" or null if unknown. */
        String batteryInfo();
    }

    private final Host host;
    private volatile boolean alive;
    private ServerSocket serverSocket;
    private DatagramSocket udp;
    private Thread acceptThread, udpThread;
    private volatile Client current;

    StreamServer(Host host) { this.host = host; }

    void start() {
        if (alive) return;
        alive = true;
        acceptThread = new Thread(new Runnable() { public void run() { acceptLoop(); } }, "accept");
        acceptThread.start();
        udpThread = new Thread(new Runnable() { public void run() { udpLoop(); } }, "discovery");
        udpThread.start();
    }

    void stop() {
        alive = false;
        try { if (serverSocket != null) serverSocket.close(); } catch (IOException ignored) {}
        try { if (udp != null) udp.close(); } catch (Exception ignored) {}
        Client c = current;
        if (c != null) c.close();
    }

    /** Torch / zoom changed on the phone's own screen: apply and tell the PC so its controls follow. */
    void localTorch(boolean on) {
        Client c = current;
        if (c != null) c.localTorch(on);
    }

    void localZoom(float z) {
        Client c = current;
        if (c != null) c.localZoom(z);
    }

    boolean streaming() { return current != null; }

    /** True while streaming from the front camera (torch button = screen light). */
    boolean frontCamera() {
        Client c = current;
        return c != null && c.isFront();
    }

    /** Rotation changed on the phone, tell the PC. */
    void notifyRotation(int degrees) {
        Client c = current;
        if (c != null) c.sendRotation(degrees);
    }

    private void acceptLoop() {
        try {
            serverSocket = new ServerSocket();
            serverSocket.setReuseAddress(true);
            serverSocket.bind(new InetSocketAddress(Protocol.TCP_PORT));
        } catch (IOException e) {
            host.onStatus("Порт " + Protocol.TCP_PORT + " занят: " + e.getMessage(), false);
            return;
        }
        host.onStatus("Ждём подключения с ПК…", false);
        while (alive) {
            try {
                final Socket s = serverSocket.accept();
                new Thread(new Runnable() { public void run() { handshake(s); } }, "client").start();
            } catch (IOException e) {
                if (alive) Log.w(TAG, "accept", e);
            }
        }
    }

    /** First line decides: capability query (always answered) or a stream (one at a time). */
    private void handshake(Socket s) {
        try {
            s.setSoTimeout(15000);
            InputStream in = s.getInputStream();
            String line = readLine(in);
            if (line == null) { s.close(); return; }
            if (line.startsWith("PHC1 CAPS")) {
                OutputStream o = s.getOutputStream();
                o.write((host.streamer().capabilities() + "\n").getBytes("UTF-8"));
                o.flush();
                s.close();
                return;
            }
            Client c;
            synchronized (this) {
                if (current != null) {
                    OutputStream o = s.getOutputStream();
                    o.write("PHC1 ERR телефон уже занят другим подключением\n".getBytes("UTF-8"));
                    o.flush();
                    s.close();
                    return;
                }
                c = new Client(s);
                current = c;
            }
            c.run(line, in);
        } catch (IOException e) {
            try { s.close(); } catch (IOException ignored) {}
        }
    }

    private void udpLoop() {
        try {
            udp = new DatagramSocket(null);
            udp.setReuseAddress(true);
            udp.setBroadcast(true);
            udp.bind(new InetSocketAddress(Protocol.UDP_PORT));
            byte[] buf = new byte[256];
            while (alive) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                udp.receive(p);
                String msg = new String(p.getData(), 0, p.getLength(), "UTF-8").trim();
                if (msg.startsWith("PHC_DISCOVER")) {
                    String name = (Build.MANUFACTURER + " " + Build.MODEL).trim().replace('\n', ' ');
                    byte[] out = ("PHC_HERE " + Protocol.TCP_PORT + " " + name).getBytes("UTF-8");
                    udp.send(new DatagramPacket(out, out.length, p.getSocketAddress()));
                }
            }
        } catch (Exception e) {
            if (alive) Log.w(TAG, "udp", e);
        }
    }

    private static String readLine(InputStream in) throws IOException {
        StringBuilder sb = new StringBuilder();
        while (true) {
            int b = in.read();
            if (b < 0) return sb.length() > 0 ? sb.toString() : null;
            if (b == '\n') return sb.toString().trim();
            if (sb.length() > 512) throw new IOException("line too long");
            sb.append((char) b);
        }
    }

    private static final class Packet {
        final int type; final long pts; final byte[] data;
        final long queuedAt = System.nanoTime();
        Packet(int type, long pts, byte[] data) { this.type = type; this.pts = pts; this.data = data; }
    }

    private final class Client implements CameraStreamer.Sink {
        private final Socket socket;
        private final LinkedBlockingDeque<Packet> queue = new LinkedBlockingDeque<Packet>();
        private volatile boolean open = true;
        private volatile boolean waitKey = true;
        private byte[] config;
        private DataOutputStream out;
        private CameraStreamer.Started started;
        private CameraStreamer.Request request;
        private float zoomValue = 1f;
        private boolean torchOn;

        boolean isFront() { return started != null && started.front; }

        void localTorch(boolean on) {
            torchOn = on;
            onTorch(on);
            sendInfo("torch " + (on ? 1 : 0));
        }

        void localZoom(float z) {
            zoomValue = z;
            host.streamer().setZoom(z);
            sendInfo(String.format(java.util.Locale.US, "zoom %.1f", z));
        }

        private void sendInfo(String text) {
            try {
                queue.offerFirst(new Packet(Protocol.PKT_INFO, 0, text.getBytes("UTF-8")));
            } catch (Exception ignored) {}
        }
        private volatile boolean torchFallback;   // running at 30 FPS because the torch didn't light in 60 FPS mode
        private final Object modeLock = new Object();
        private int sentFrames, droppedFrames, keyRequests;
        private long phoneLatSum; private int phoneLatN;
        // adaptive bitrate: go down when the network falls behind, creep back up when it is stable
        private int targetKbps, currentKbps;
        private long lastCongestion;
        private int congestions;
        private static final long MAX_QUEUE_AGE_NS = 150_000_000L; // 150 ms
        private long sentBytes;
        private String peerName = "";
        private int reqFps;

        Client(Socket s) { socket = s; }

        void run(String firstLine, InputStream in) {
            String peer = String.valueOf(socket.getInetAddress()).replace("/", "");
            try {
                socket.setTcpNoDelay(true);
                // Small kernel buffer: backlog stays in our queue, where old frames can be dropped.
                socket.setSendBufferSize(96 * 1024);
                socket.setSoTimeout(15000);
                out = new DataOutputStream(new BufferedOutputStream(socket.getOutputStream(), 256 * 1024));

                String line = firstLine;
                if (line == null || !line.startsWith("PHC1")) throw new IOException("bad hello: " + line);
                String[] p = line.split("\\s+");
                CameraStreamer.Request r = new CameraStreamer.Request();
                if (p.length > 1) r.width = parse(p[1], 1280);
                if (p.length > 2) r.height = parse(p[2], 720);
                if (p.length > 3) r.fps = Math.max(5, Math.min(60, parse(p[3], 30)));
                if (p.length > 4) r.kbps = Math.max(500, Math.min(50000, parse(p[4], 6000)));
                if (p.length > 5) r.camera = p[5];
                for (int i = 6; i < p.length; i++) if (p[i].equals("hs")) r.highSpeed = true;

                host.onStatus("Подключается " + peer + "…", false);
                try {
                    started = host.startStream(r, this);
                    request = r;
                } catch (Exception e) {
                    String msg = e.getMessage() != null ? e.getMessage() : e.toString();
                    out.write(("PHC1 ERR " + msg.replace('\n', ' ') + "\n").getBytes("UTF-8"));
                    out.flush();
                    throw e;
                }
                int rot = host.currentRotation(started);
                out.write(("PHC1 OK " + started.width + " " + started.height + " " + rot + " " + started.fps + " "
                        + started.maxFps + (started.note.isEmpty() ? "" : " " + started.note.replace('\n', ' ')) + "\n").getBytes("UTF-8"));
                out.flush();
                peerName = peer;
                reqFps = r.fps;
                targetKbps = currentKbps = r.kbps;
                lastCongestion = System.nanoTime();
                host.onStatus("Трансляция на " + peer + "  •  " + started.width + "×" + started.height
                        + " " + started.fps + " fps", true);

                final CameraStreamer csx = host.streamer();
                csx.onEvEffectiveChanged = new Runnable() {
                    public void run() {
                        try {
                            byte[] d = ("evhw " + (csx.evEffective ? 1 : 0)).getBytes("UTF-8");
                            queue.offerFirst(new Packet(Protocol.PKT_INFO, 0, d));
                        } catch (Exception ignored) {}
                    }
                };
                Thread writer = new Thread(new Runnable() { public void run() { writeLoop(); } }, "writer");
                writer.start();
                host.streamer().requestKeyFrame();

                socket.setSoTimeout(0);
                while (open) {
                    String cmd = readLine(in);
                    if (cmd == null) break;
                    handleCommand(cmd);
                }
            } catch (Exception e) {
                Log.i(TAG, "client ended: " + e);
            } finally {
                close();
                if (started != null) host.stopStream();
                current = null;
                if (alive) host.onStatus("ПК отключился. Ждём подключения…", false);
            }
        }

        private int parse(String s, int def) {
            try { return Integer.parseInt(s); } catch (NumberFormatException e) { return def; }
        }

        private void handleCommand(String cmd) {
            String[] p = cmd.split("\\s+");
            CameraStreamer cs = host.streamer();
            try {
                if (p[0].equals("TORCH") && p.length > 1) {
                    torchOn = p[1].equals("1");
                    onTorch(torchOn);
                    host.onRemoteControl(torchOn, zoomValue);
                } else if (p[0].equals("ZOOM") && p.length > 1) {
                    zoomValue = Float.parseFloat(p[1]);
                    cs.setZoom(zoomValue);
                    host.onRemoteControl(torchOn, zoomValue);
                }
                else if (p[0].equals("KEYFRAME")) { keyRequests++; cs.requestKeyFrame(); }
                else if (p[0].equals("AF")) cs.triggerAutofocus();
                else if (p[0].equals("ORIENT") && p.length > 1) host.requestOrientation(p[1].equals("p"));
                else if (p[0].equals("EV") && p.length > 1) cs.setExposure(Float.parseFloat(p[1]));
                else if (p[0].equals("WB") && p.length > 1) cs.setWhiteBalance(Integer.parseInt(p[1]));
                else if (p[0].equals("EDGE") && p.length > 1) cs.setEdge(Integer.parseInt(p[1]));
                else if (p[0].equals("NR") && p.length > 1) cs.setNoiseReduction(Integer.parseInt(p[1]));
                else if (p[0].equals("AELOCK") && p.length > 1) cs.setAeLock(p[1].equals("1"));
                else if (p[0].equals("AWBLOCK") && p.length > 1) cs.setAwbLock(p[1].equals("1"));
                else if (p[0].equals("PING") && p.length > 1) {
                    // goes to the front of the queue so the backlog doesn't distort the clock sync
                    byte[] d = (p[1] + " " + cs.nowUs()).getBytes("UTF-8");
                    queue.offerFirst(new Packet(Protocol.PKT_PONG, 0, d));
                }
            } catch (Exception e) {
                Log.w(TAG, "cmd " + cmd, e);
            }
        }

        /**
         * Torch in 60 FPS (high-speed) mode: try it there first; if the camera doesn't light it within a second,
         * restart the camera in normal 30 FPS mode with the torch, and go back to 60 FPS when it's switched off.
         */
        private void onTorch(final boolean on) {
            final CameraStreamer cs = host.streamer();
            if (started != null && started.front) {
                host.setScreenLight(on);
                return;
            }
            if (on) {
                cs.setTorch(true);
                if (!cs.isHighSpeed()) return;
                new Thread(new Runnable() {
                    public void run() {
                        // Fall back to 30 FPS ONLY if the camera explicitly rejected the torch request.
                        // Guessing from the camera's flash state was unreliable: some cameras light the torch
                        // but report it late or inconsistently, which caused false switches to 30 FPS.
                        long start = System.currentTimeMillis();
                        boolean fallback = false;
                        // wait for the re-asserts (0.4 s and 1.2 s), then judge by the latest attempt
                        while (open && cs.isTorchRequested() && cs.isHighSpeed()
                                && System.currentTimeMillis() - start < 1800) {
                            try { Thread.sleep(100); } catch (InterruptedException e) { return; }
                        }
                        fallback = cs.torchRejected();
                        if (!fallback || !open || !cs.isTorchRequested() || !cs.isHighSpeed()) return;
                        Log.i(TAG, "torch didn't light in high-speed mode, switching to normal mode");
                        torchFallback = true;
                        switchMode(false, true);
                    }
                }, "torch-check").start();
            } else {
                cs.setTorch(false);
                if (torchFallback) {
                    torchFallback = false;
                    new Thread(new Runnable() { public void run() { switchMode(true, false); } }, "mode").start();
                }
            }
        }

        /** Restart the camera with/without high-speed mode on the same connection. */
        private void switchMode(boolean highSpeed, boolean torchOn) {
            synchronized (modeLock) {
                if (!open || request == null) return;
                try {
                    request.highSpeed = highSpeed;
                    host.stopStream();
                    waitKey = true;
                    started = host.startStream(request, this);
                    CameraStreamer cs = host.streamer();
                    if (torchOn) cs.setTorch(true);
                    if (zoomValue > 1.01f) cs.setZoom(zoomValue);
                    host.reapplyCameraSettings();
                    sendRotation(host.currentRotation(started));
                    cs.requestKeyFrame();
                } catch (Exception e) {
                    Log.w(TAG, "mode switch failed", e);
                    host.onStatus("Ошибка переключения режима: " + e.getMessage(), false);
                    close();
                }
            }
        }

        @Override public void onPacket(int type, long ptsUs, byte[] data) {
            if (!open) return;
            if (type == Protocol.PKT_CONFIG) {
                config = data;
                queue.offer(new Packet(type, ptsUs, data));
                return;
            }
            // If the network can't keep up (oldest queued frame too old), drop the backlog,
            // restart from a fresh key frame and lower the bitrate.
            Packet head = queue.peekFirst();
            if (head != null && System.nanoTime() - head.queuedAt > MAX_QUEUE_AGE_NS) {
                droppedFrames += queue.size();
                keyRequests++;
                queue.clear();
                waitKey = true;
                congestion();
                host.streamer().requestKeyFrame();
            }
            if (waitKey) {
                if (type != Protocol.PKT_KEY) { droppedFrames++; return; }
                waitKey = false;
                if (config != null) queue.offer(new Packet(Protocol.PKT_CONFIG, ptsUs, config));
            }
            queue.offer(new Packet(type, ptsUs, data));
        }

        private void congestion() {
            congestions++;
            lastCongestion = System.nanoTime();
            int next = Math.max(Math.max(500, targetKbps / 6), (int) (currentKbps * 0.7));
            if (next < currentKbps) {
                currentKbps = next;
                host.streamer().setBitrate(currentKbps);
            }
        }

        /** Called once a second: raise the bitrate back towards the target after 4 s without problems. */
        private void maybeRaiseBitrate() {
            if (currentKbps >= targetKbps) return;
            if (System.nanoTime() - lastCongestion < 4_000_000_000L) return;
            currentKbps = Math.min(targetKbps, (int) (currentKbps * 1.15) + 100);
            host.streamer().setBitrate(currentKbps);
        }

        void sendRotation(int degrees) {
            if (!open) return;
            queue.offer(new Packet(Protocol.PKT_ROTATION, 0, new byte[]{(byte) (degrees >> 8), (byte) degrees}));
        }

        @Override public void onStreamError(String message) {
            host.onStatus("Ошибка: " + message, false);
            close();
        }

        private void writeLoop() {
            long statT0 = System.currentTimeMillis();
            long batT0 = 0;
            String lastBat = null;
            CameraStreamer cs = host.streamer();
            try {
                while (open) {
                    Packet pk = queue.poll(250, TimeUnit.MILLISECONDS);
                    if (pk != null) {
                        synchronized (this) {
                            writePacket(pk.type, pk.pts, pk.data);
                            if (queue.isEmpty()) out.flush();
                        }
                        if (pk.type == Protocol.PKT_KEY || pk.type == Protocol.PKT_FRAME) {
                            sentFrames++;
                            sentBytes += pk.data.length;
                            long lat = cs.nowUs() - pk.pts;
                            if (lat >= 0 && lat < 10_000_000L) { phoneLatSum += lat; phoneLatN++; }
                        }
                    }
                    long now = System.currentTimeMillis();
                    if (now - batT0 >= 5000) {
                        batT0 = now;
                        String b = host.batteryInfo();
                        if (b != null && !b.equals(lastBat)) {
                            lastBat = b;
                            synchronized (this) {
                                writePacket(Protocol.PKT_INFO, 0, b.getBytes("UTF-8"));
                                out.flush();
                            }
                        }
                    }
                    if (now - statT0 >= 1000) {
                        double sec = (now - statT0) / 1000.0;
                        String st = String.format(java.util.Locale.US,
                                "камера %.0f к/с (диапазон %s), кодер %.0f, отправлено %.0f (%.1f Мбит/с), сброшено %d, очередь %d%s",
                                cs.cameraFrames.getAndSet(0) / sec, cs.fpsRangeText,
                                cs.encodedFrames.getAndSet(0) / sec, sentFrames / sec, sentBytes * 8 / sec / 1e6,
                                droppedFrames, queue.size(),
                                keyRequests > 0 ? ", запросов ключ. кадра " + keyRequests : "");
                        maybeRaiseBitrate();
                        if (phoneLatN > 0) st += String.format(java.util.Locale.US, ", на телефоне %d мс", phoneLatSum / phoneLatN / 1000);
                        phoneLatSum = 0; phoneLatN = 0;
                        if (currentKbps < targetKbps)
                            st += String.format(java.util.Locale.US, ", битрейт снижен до %.1f из %.1f Мбит/с (сеть не тянет)",
                                    currentKbps / 1000.0, targetKbps / 1000.0);
                        if (!cs.controlsInfo.isEmpty()) st += ", " + cs.controlsInfo;
                        String fst = cs.focusStatus();
                        if (!fst.isEmpty()) st += ", " + fst;
                        String ts = cs.torchStatus();
                        if (torchFallback) ts = "фонарик горит, временно 30 FPS (в режиме 60 FPS камера его не включает)";
                        if (!ts.isEmpty()) st += ", " + ts;
                        st += ", кодер: " + cs.encoderInfo;
                        int fails = cs.captureFailures.getAndSet(0);
                        if (fails > 0) st += ", сбоев камеры " + fails;
                        sentFrames = 0; sentBytes = 0; droppedFrames = 0; keyRequests = 0;
                        statT0 = now;
                        synchronized (this) {
                            writePacket(Protocol.PKT_STATS, 0, st.getBytes("UTF-8"));
                            out.flush();
                        }
                        host.onStatus("Трансляция на " + peerName + "\n" + st, true);
                    }
                }
            } catch (Exception e) {
                Log.i(TAG, "writer ended: " + e);
            } finally {
                close();
            }
        }

        private void writePacket(int type, long pts, byte[] data) throws IOException {
            out.writeInt(data.length);
            out.writeByte(type);
            out.writeLong(pts);
            out.write(data);
        }

        void close() {
            open = false;
            try { socket.close(); } catch (IOException ignored) {}
        }
    }
}
