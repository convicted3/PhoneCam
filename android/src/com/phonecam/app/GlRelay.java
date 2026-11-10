package com.phonecam.app;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Experimental high-speed relay: the camera renders into our SurfaceTexture at e.g. 120 fps,
 * we draw only every N-th frame into the encoder's input surface (e.g. 60 fps).
 */
final class GlRelay implements SurfaceTexture.OnFrameAvailableListener {
    private static final String TAG = "PhoneCam";
    private static final int EGL_RECORDABLE_ANDROID = 0x3142;

    private static final String VS =
            "attribute vec4 aPos;\n" +
            "attribute vec4 aTex;\n" +
            "uniform mat4 uTexM;\n" +
            "varying vec2 vTex;\n" +
            "void main() { gl_Position = aPos; vTex = (uTexM * aTex).xy; }\n";
    private static final String FS =
            "#extension GL_OES_EGL_image_external : require\n" +
            "precision mediump float;\n" +
            "varying vec2 vTex;\n" +
            "uniform samplerExternalOES sTex;\n" +
            "void main() { gl_FragColor = texture2D(sTex, vTex); }\n";

    private final int width, height, keepEvery;
    private final Surface encoderSurface;
    private HandlerThread thread;
    private Handler handler;
    private EGLDisplay display = EGL14.EGL_NO_DISPLAY;
    private EGLContext context = EGL14.EGL_NO_CONTEXT;
    private EGLSurface eglSurface = EGL14.EGL_NO_SURFACE;
    private int program, texId, aPos, aTex, uTexM;
    private SurfaceTexture cameraTexture;
    private Surface cameraSurface;
    // Raw buffer orientation (= what the camera would write straight into the encoder), only flip V.
    // We deliberately ignore SurfaceTexture.getTransformMatrix(): it may contain the sensor rotation.
    private final float[] texMatrix = {1f, 0f, 0f, 0f, 0f, -1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 1f, 0f, 1f};
    private FloatBuffer quad;
    private long frameIndex;
    private volatile boolean released;
    final AtomicInteger relayed = new AtomicInteger();

    GlRelay(Surface encoderSurface, int width, int height, int keepEvery) {
        this.encoderSurface = encoderSurface;
        this.width = width;
        this.height = height;
        this.keepEvery = Math.max(1, keepEvery);
    }

    /** Sets up EGL on its own thread. Returns the Surface the camera should render into. */
    Surface start() throws Exception {
        thread = new HandlerThread("gl-relay");
        thread.start();
        handler = new Handler(thread.getLooper());
        final CountDownLatch done = new CountDownLatch(1);
        final Exception[] err = new Exception[1];
        handler.post(new Runnable() {
            public void run() {
                try { setup(); } catch (Exception e) { err[0] = e; }
                done.countDown();
            }
        });
        if (!done.await(5, TimeUnit.SECONDS)) throw new Exception(L.t("GL не запустился"));
        if (err[0] != null) { release(); throw err[0]; }
        return cameraSurface;
    }

    private void setup() throws Exception {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] ver = new int[2];
        if (!EGL14.eglInitialize(display, ver, 0, ver, 1)) throw new Exception("eglInitialize");
        int[] attribs = {
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL_RECORDABLE_ANDROID, 1,
                EGL14.EGL_NONE};
        EGLConfig[] cfgs = new EGLConfig[1];
        int[] num = new int[1];
        if (!EGL14.eglChooseConfig(display, attribs, 0, cfgs, 0, 1, num, 0) || num[0] == 0)
            throw new Exception("eglChooseConfig");
        int[] ctxAttr = {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE};
        context = EGL14.eglCreateContext(display, cfgs[0], EGL14.EGL_NO_CONTEXT, ctxAttr, 0);
        if (context == EGL14.EGL_NO_CONTEXT) throw new Exception("eglCreateContext");
        eglSurface = EGL14.eglCreateWindowSurface(display, cfgs[0], encoderSurface, new int[]{EGL14.EGL_NONE}, 0);
        if (eglSurface == EGL14.EGL_NO_SURFACE) throw new Exception("eglCreateWindowSurface");
        if (!EGL14.eglMakeCurrent(display, eglSurface, eglSurface, context)) throw new Exception("eglMakeCurrent");

        program = link(VS, FS);
        aPos = GLES20.glGetAttribLocation(program, "aPos");
        aTex = GLES20.glGetAttribLocation(program, "aTex");
        uTexM = GLES20.glGetUniformLocation(program, "uTexM");

        int[] t = new int[1];
        GLES20.glGenTextures(1, t, 0);
        texId = t[0];
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);

        float[] v = { // x, y, u, v  (triangle strip)
                -1f, -1f, 0f, 0f,
                1f, -1f, 1f, 0f,
                -1f, 1f, 0f, 1f,
                1f, 1f, 1f, 1f};
        quad = ByteBuffer.allocateDirect(v.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        quad.put(v).position(0);

        cameraTexture = new SurfaceTexture(texId);
        cameraTexture.setDefaultBufferSize(width, height);
        cameraTexture.setOnFrameAvailableListener(this, handler);
        cameraSurface = new Surface(cameraTexture);
    }

    private static int compile(int type, String src) throws Exception {
        int s = GLES20.glCreateShader(type);
        GLES20.glShaderSource(s, src);
        GLES20.glCompileShader(s);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) throw new Exception("shader: " + GLES20.glGetShaderInfoLog(s));
        return s;
    }

    private static int link(String vs, String fs) throws Exception {
        int p = GLES20.glCreateProgram();
        GLES20.glAttachShader(p, compile(GLES20.GL_VERTEX_SHADER, vs));
        GLES20.glAttachShader(p, compile(GLES20.GL_FRAGMENT_SHADER, fs));
        GLES20.glLinkProgram(p);
        int[] ok = new int[1];
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0);
        if (ok[0] == 0) throw new Exception("link: " + GLES20.glGetProgramInfoLog(p));
        return p;
    }

    @Override
    public void onFrameAvailable(SurfaceTexture st) {
        if (released) return;
        try {
            st.updateTexImage();
            if ((frameIndex++ % keepEvery) != 0) return; // drop this one
            GLES20.glViewport(0, 0, width, height);
            GLES20.glUseProgram(program);
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId);
            quad.position(0);
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 16, quad);
            GLES20.glEnableVertexAttribArray(aPos);
            quad.position(2);
            GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 16, quad);
            GLES20.glEnableVertexAttribArray(aTex);
            GLES20.glUniformMatrix4fv(uTexM, 1, false, texMatrix, 0);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
            EGLExt.eglPresentationTimeANDROID(display, eglSurface, st.getTimestamp());
            EGL14.eglSwapBuffers(display, eglSurface);
            relayed.incrementAndGet();
        } catch (Exception e) {
            Log.w(TAG, "relay frame", e);
        }
    }

    void release() {
        released = true;
        if (handler == null) return;
        final CountDownLatch done = new CountDownLatch(1);
        handler.post(new Runnable() {
            public void run() {
                try {
                    if (cameraTexture != null) { cameraTexture.setOnFrameAvailableListener(null); cameraTexture.release(); }
                    if (cameraSurface != null) cameraSurface.release();
                    if (display != EGL14.EGL_NO_DISPLAY) {
                        EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                        if (eglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, eglSurface);
                        if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context);
                        EGL14.eglReleaseThread(); // no eglTerminate: the display is shared with the UI renderer
                    }
                } catch (Exception ignored) {}
                done.countDown();
            }
        });
        try { done.await(2, TimeUnit.SECONDS); } catch (InterruptedException ignored) {}
        thread.quitSafely();
        handler = null;
    }
}
