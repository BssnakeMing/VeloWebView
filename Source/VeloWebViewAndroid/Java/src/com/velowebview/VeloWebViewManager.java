package com.velowebview;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Rect;
import android.net.Uri;
import android.net.http.SslError;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.webkit.CookieManager;
import android.webkit.CookieSyncManager;
import android.webkit.DownloadListener;
import android.webkit.GeolocationPermissions;
import android.webkit.JavascriptInterface;
import android.webkit.JsResult;
import android.webkit.PermissionRequest;
import android.webkit.SslErrorHandler;
import android.webkit.URLUtil;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import java.io.UnsupportedEncodingException;
import java.net.URL;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Native Android overlay manager for VeloWebView.
 *
 * The class is intentionally static: UE can own any number of WebView
 * instances while a single Activity owns the overlay root.  Every operation
 * is marshalled to the Android main/UI thread and every native callback is
 * caught so a missing optional native library cannot crash the app.
 */
@SuppressWarnings("deprecation")
public final class VeloWebViewManager {
    private static final String TAG = "VeloWebView";
    private static final Object LOCK = new Object();
    private static final AtomicInteger NEXT_ID = new AtomicInteger(1);
    private static final AtomicInteger NEXT_PERMISSION_TOKEN = new AtomicInteger(1);
    private static final int RUNTIME_PERMISSION_REQUEST_BASE = 0x5E00;
    // Keep these values in sync with EVeloWebViewNewWindowPolicy.
    private static final int NEW_WINDOW_SAME_VIEW = 0;
    private static final int NEW_WINDOW_EXTERNAL = 1;
    private static final int NEW_WINDOW_NOTIFY_ONLY = 2;
    private static final int NEW_WINDOW_BLOCK = 3;
    private static final Handler MAIN_HANDLER = new Handler(Looper.getMainLooper());

    private static volatile Activity activity;
    private static FrameLayout overlayRoot;
    private static final Map<Integer, Instance> INSTANCES = new HashMap<Integer, Instance>();
    private static final HashSet<Integer> RESERVED_IDS = new HashSet<Integer>();
    private static final Map<Integer, PendingPermission> PENDING_PERMISSIONS = new HashMap<Integer, PendingPermission>();
    private static final Map<Integer, Integer> RUNTIME_REQUEST_TO_TOKEN = new HashMap<Integer, Integer>();
    private static boolean nativeUnavailable;

    private VeloWebViewManager() { }

    private static void log(String message) {
        // Avoid depending on android.util.Log in release builds; UE receives
        // browser console messages through the native callback instead.
        if (Build.VERSION.SDK_INT >= 1) {
            android.util.Log.d(TAG, message);
        }
    }

    /** Called by the JNI bridge and by GameActivity onCreate additions. */
    public static void initialize(final Activity newActivity) {
        if (newActivity == null) {
            return;
        }
        // Publish the host immediately so a JNI call sequence of
        // initialize(); create() from the Unreal game thread can enqueue both
        // operations in order on the UI looper.
        activity = newActivity;
        runOnUiThread(newActivity, new Runnable() {
            @Override public void run() {
                attachActivityOnUiThread(newActivity);
            }
        });
    }

    private static void attachActivityOnUiThread(Activity newActivity) {
        if (activity == newActivity && overlayRoot != null) {
            return;
        }

        FrameLayout newRoot = resolveOverlayRoot(newActivity);
        if (newRoot == null) {
            return;
        }

        if (overlayRoot != null && overlayRoot.getParent() instanceof ViewGroup) {
            ((ViewGroup) overlayRoot.getParent()).removeView(overlayRoot);
        }

        activity = newActivity;
        overlayRoot = new FrameLayout(newActivity);
        overlayRoot.setClipChildren(false);
        overlayRoot.setClipToPadding(false);
        overlayRoot.setBackgroundColor(Color.TRANSPARENT);
        overlayRoot.setClickable(false);
        overlayRoot.setFocusable(false);
        FrameLayout.LayoutParams rootParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        rootParams.gravity = Gravity.TOP | Gravity.START;
        newRoot.addView(overlayRoot, rootParams);
        overlayRoot.bringToFront();

        // Activity recreation: reparent surviving instances to the new root.
        synchronized (LOCK) {
            for (Instance instance : INSTANCES.values()) {
                if (instance.webView.getParent() instanceof ViewGroup) {
                    ((ViewGroup) instance.webView.getParent()).removeView(instance.webView);
                }
                if (instance.fullscreenView == null) {
                    overlayRoot.addView(instance.webView, instance.layoutParams());
                }
            }
        }
    }

    private static FrameLayout resolveOverlayRoot(Activity host) {
        View content = host.findViewById(android.R.id.content);
        if (content instanceof FrameLayout) {
            return (FrameLayout) content;
        }
        if (content instanceof ViewGroup) {
            FrameLayout wrapper = new FrameLayout(host);
            ViewGroup parent = (ViewGroup) content;
            int index = parent.indexOfChild(content);
            // A content view is normally a FrameLayout.  Keep a safe fallback
            // for custom hosts without disturbing their existing child order.
            if (index >= 0) {
                parent.removeView(content);
                wrapper.addView(content, new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
                parent.addView(wrapper, index);
            }
            return wrapper;
        }
        View decor = host.getWindow() == null ? null : host.getWindow().getDecorView();
        return decor instanceof FrameLayout ? (FrameLayout) decor : null;
    }

    private static void runOnUiThread(Activity host, Runnable runnable) {
        if (host == null || runnable == null) {
            return;
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            runnable.run();
        } else {
            host.runOnUiThread(runnable);
        }
    }

    private static void runOnUiThread(Runnable runnable) {
        Activity host = activity;
        if (host != null) {
            runOnUiThread(host, runnable);
        } else {
            MAIN_HANDLER.post(runnable);
        }
    }

    private static Instance getInstance(int id) {
        synchronized (LOCK) {
            return INSTANCES.get(id);
        }
    }

    /**
     * Creates an instance.  The returned ID is allocated synchronously; the
     * actual WebView is constructed on the UI thread before subsequent calls
     * are executed.  Requested IDs are honoured when unique and positive.
     */
    public static int create(final int requestedId, final boolean enableJavaScript,
                              final boolean mediaAutoplay, final boolean muted,
                              final boolean transparentBackground,
                              final boolean allowMixedContent, final String userAgent) {
        final int id;
        synchronized (LOCK) {
            int candidate = requestedId > 0 ? requestedId : NEXT_ID.getAndIncrement();
            while (INSTANCES.containsKey(candidate) || RESERVED_IDS.contains(candidate)) {
                candidate = NEXT_ID.getAndIncrement();
            }
            RESERVED_IDS.add(candidate);
            id = candidate;
        }

        final Activity host = activity;
        if (host == null) {
            synchronized (LOCK) { RESERVED_IDS.remove(id); }
            return 0;
        }

        runOnUiThread(host, new Runnable() {
            @Override public void run() {
                createOnUiThread(id, enableJavaScript, mediaAutoplay, muted,
                        transparentBackground, allowMixedContent, userAgent);
            }
        });
        return id;
    }

    private static void createOnUiThread(int id, boolean enableJavaScript,
                                         boolean mediaAutoplay, boolean muted,
                                         boolean transparentBackground,
                                         boolean allowMixedContent, String userAgent) {
        if (overlayRoot == null || activity == null) {
            synchronized (LOCK) { RESERVED_IDS.remove(id); }
            return;
        }
        synchronized (LOCK) {
            if (INSTANCES.containsKey(id)) {
                RESERVED_IDS.remove(id);
                return;
            }
        }
        Instance instance = new Instance(id, activity, enableJavaScript, mediaAutoplay,
                muted, transparentBackground, allowMixedContent, userAgent);
        synchronized (LOCK) {
            INSTANCES.put(id, instance);
            RESERVED_IDS.remove(id);
        }
        overlayRoot.addView(instance.webView, instance.layoutParams());
        instance.webView.setVisibility(View.INVISIBLE);
    }

    public static void destroy(final int id) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                destroyOnUiThread(id);
            }
        });
    }

    private static void destroyOnUiThread(int id) {
        Instance instance;
        synchronized (LOCK) {
            instance = INSTANCES.remove(id);
            RESERVED_IDS.remove(id);
        }
        if (instance == null) {
            return;
        }
        instance.destroy();
    }

    public static void setBounds(final int id, final float x, final float y,
                                 final float width, final float height,
                                 final boolean coordinatesAreDp) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                Instance instance = getInstance(id);
                if (instance != null) {
                    instance.setBounds(x, y, width, height, coordinatesAreDp);
                }
            }
        });
    }

    public static void setVisibility(final int id, final boolean visible) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                Instance instance = getInstance(id);
                if (instance != null && instance.fullscreenView == null) {
                    instance.webView.setVisibility(visible ? View.VISIBLE : View.INVISIBLE);
                }
            }
        });
    }

    public static void setFocus(final int id, final boolean focused) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                Instance instance = getInstance(id);
                if (instance != null) {
                    if (focused) {
                        instance.webView.setFocusableInTouchMode(true);
                        instance.webView.requestFocus(View.FOCUS_DOWN);
                    } else {
                        instance.webView.clearFocus();
                    }
                }
            }
        });
    }

    public static void loadURL(final int id, final String url) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                Instance instance = getInstance(id);
                if (instance != null && url != null) {
                    instance.webView.loadUrl(url);
                }
            }
        });
    }

    public static void loadHTML(final int id, final String html, final String baseUrl) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                Instance instance = getInstance(id);
                if (instance != null) {
                    instance.webView.loadDataWithBaseURL(baseUrl == null ? "about:blank" : baseUrl,
                            html == null ? "" : html, "text/html", "UTF-8", null);
                }
            }
        });
    }

    public static void reload(final int id) { invoke(id, new InstanceAction() { @Override public void run(Instance i) { i.webView.reload(); } }); }
    public static void stop(final int id) { invoke(id, new InstanceAction() { @Override public void run(Instance i) { i.webView.stopLoading(); } }); }
    public static void goBack(final int id) { invoke(id, new InstanceAction() { @Override public void run(Instance i) { if (i.webView.canGoBack()) i.webView.goBack(); } }); }
    public static void goForward(final int id) { invoke(id, new InstanceAction() { @Override public void run(Instance i) { if (i.webView.canGoForward()) i.webView.goForward(); } }); }
    public static boolean canGoBack(final int id) { return query(id, new InstanceQuery<Boolean>() { @Override public Boolean run(Instance i) { return i.webView.canGoBack(); } }, false); }
    public static boolean canGoForward(final int id) { return query(id, new InstanceQuery<Boolean>() { @Override public Boolean run(Instance i) { return i.webView.canGoForward(); } }, false); }

    public static void executeJavaScript(final int id, final String script) {
        invoke(id, new InstanceAction() { @Override public void run(Instance i) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
                i.webView.evaluateJavascript(script == null ? "" : script, null);
            } else {
                i.webView.loadUrl("javascript:" + (script == null ? "" : script));
            }
        }});
    }

    public static void setJavaScriptEnabled(final int id, final boolean enabled) {
        invoke(id, new InstanceAction() { @Override public void run(Instance i) { i.setJavaScriptEnabled(enabled); } });
    }
    public static void setMediaAutoplay(final int id, final boolean enabled) {
        invoke(id, new InstanceAction() { @Override public void run(Instance i) { i.setMediaAutoplay(enabled); } });
    }
    public static void setMuted(final int id, final boolean muted) {
        invoke(id, new InstanceAction() { @Override public void run(Instance i) { i.setMuted(muted); } });
    }
    public static void setUserAgent(final int id, final String userAgent) {
        invoke(id, new InstanceAction() { @Override public void run(Instance i) { i.setUserAgent(userAgent); } });
    }
    public static void setBackgroundTransparent(final int id, final boolean transparent) {
        invoke(id, new InstanceAction() { @Override public void run(Instance i) { i.setBackgroundTransparent(transparent); } });
    }
    public static void setAllowMixedContent(final int id, final boolean allowed) {
        invoke(id, new InstanceAction() { @Override public void run(Instance i) { i.setAllowMixedContent(allowed); } });
    }
    public static void setNewWindowPolicy(final int id, final int policy) {
        invoke(id, new InstanceAction() { @Override public void run(Instance i) { i.newWindowPolicy = policy; } });
    }

    public static String getURL(final int id) {
        return query(id, new InstanceQuery<String>() { @Override public String run(Instance i) { return i.webView.getUrl(); } }, "");
    }
    public static String getTitle(final int id) {
        return query(id, new InstanceQuery<String>() { @Override public String run(Instance i) { return i.webView.getTitle(); } }, "");
    }

    private interface InstanceAction { void run(Instance instance); }
    private interface InstanceQuery<T> { T run(Instance instance); }

    private static void invoke(final int id, final InstanceAction action) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                Instance instance = getInstance(id);
                if (instance != null && action != null) {
                    action.run(instance);
                }
            }
        });
    }

    private static <T> T query(final int id, InstanceQuery<T> query, T fallback) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            Instance instance = getInstance(id);
            return instance == null ? fallback : query.run(instance);
        }
        final CountDownLatch latch = new CountDownLatch(1);
        final Object[] result = new Object[] { fallback };
        runOnUiThread(new Runnable() {
            @Override public void run() {
                try {
                    Instance instance = getInstance(id);
                    if (instance != null) {
                        result[0] = query.run(instance);
                    }
                } finally {
                    latch.countDown();
                }
            }
        });
        try {
            latch.await(2, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        @SuppressWarnings("unchecked") T typed = (T) result[0];
        return typed;
    }

    // Cookie APIs are process-wide on Android and are therefore intentionally
    // not tied to an instance ID.
    public static void setCookie(final String url, final String cookie) {
        runOnUiThread(new Runnable() { @Override public void run() {
            if (url != null && cookie != null) {
                CookieManager.getInstance().setCookie(url, cookie);
                flushCookies();
            }
        }});
    }
    public static String getCookie(final String url) {
        if (url == null) return "";
        return queryCookie(url);
    }
    private static String queryCookie(final String url) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            String value = CookieManager.getInstance().getCookie(url);
            return value == null ? "" : value;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        final String[] result = new String[] { "" };
        runOnUiThread(new Runnable() { @Override public void run() {
            try {
                String value = CookieManager.getInstance().getCookie(url);
                result[0] = value == null ? "" : value;
            } finally { latch.countDown(); }
        }});
        try { latch.await(2, TimeUnit.SECONDS); } catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
        return result[0];
    }
    public static void deleteCookie(final String url, final String name) {
        runOnUiThread(new Runnable() { @Override public void run() {
            if (url != null && name != null) {
                CookieManager.getInstance().setCookie(url, name + "=; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Path=/");
                flushCookies();
            }
        }});
    }
    public static void clearCookies() {
        runOnUiThread(new Runnable() { @Override public void run() {
            CookieManager.getInstance().removeAllCookies(new ValueCallback<Boolean>() { @Override public void onReceiveValue(Boolean value) { flushCookies(); } });
        }});
    }
    private static void flushCookies() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) CookieManager.getInstance().flush();
        else CookieSyncManager.getInstance().sync();
    }
    public static void clearCache(final int id) {
        invoke(id, new InstanceAction() { @Override public void run(Instance i) { i.webView.clearCache(true); } });
    }

    public static void grantPermission(final int token) {
        runOnUiThread(new Runnable() { @Override public void run() { grantPermissionOnUiThread(token); } });
    }
    private static void grantPermissionOnUiThread(int token) {
        PendingPermission pending;
        synchronized (LOCK) { pending = PENDING_PERMISSIONS.get(token); }
        if (pending == null || (pending.request == null && pending.geolocationCallback == null)) return;
        ArrayList<String> runtimePermissions = runtimePermissionsFor(pending.request == null ? null : pending.request.getResources());
        if (pending.geolocationCallback != null && Build.VERSION.SDK_INT >= 23) {
            if (!runtimePermissions.contains(Manifest.permission.ACCESS_FINE_LOCATION)) runtimePermissions.add(Manifest.permission.ACCESS_FINE_LOCATION);
            if (!runtimePermissions.contains(Manifest.permission.ACCESS_COARSE_LOCATION)) runtimePermissions.add(Manifest.permission.ACCESS_COARSE_LOCATION);
        }
        if (!runtimePermissions.isEmpty() && activity != null && Build.VERSION.SDK_INT >= 23) {
            ArrayList<String> missing = new ArrayList<String>();
            for (String permission : runtimePermissions) {
                if (activity.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) missing.add(permission);
            }
            if (!missing.isEmpty()) {
                int requestCode = RUNTIME_PERMISSION_REQUEST_BASE + token;
                synchronized (LOCK) { RUNTIME_REQUEST_TO_TOKEN.put(requestCode, token); }
                activity.requestPermissions(missing.toArray(new String[missing.size()]), requestCode);
                return;
            }
        }
        grantWebPermission(token);
    }
    private static void grantWebPermission(int token) {
        PendingPermission pending;
        synchronized (LOCK) { pending = PENDING_PERMISSIONS.remove(token); }
        if (pending != null && pending.request != null) {
            pending.request.grant(pending.request.getResources());
        } else if (pending != null && pending.geolocationCallback != null) {
            pending.geolocationCallback.invoke(pending.geolocationOrigin, true, false);
        }
    }
    public static void denyPermission(final int token) {
        runOnUiThread(new Runnable() { @Override public void run() {
            PendingPermission pending;
            synchronized (LOCK) { pending = PENDING_PERMISSIONS.remove(token); }
            if (pending != null && pending.request != null) pending.request.deny();
            else if (pending != null && pending.geolocationCallback != null) pending.geolocationCallback.invoke(pending.geolocationOrigin, false, false);
        }});
    }
    public static void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode < RUNTIME_PERMISSION_REQUEST_BASE) return;
        final int token;
        synchronized (LOCK) {
            Integer value = RUNTIME_REQUEST_TO_TOKEN.remove(requestCode);
            token = value == null ? -1 : value;
        }
        if (token < 0) return;
        boolean granted = grantResults != null && grantResults.length > 0;
        if (granted) {
            for (int result : grantResults) granted &= result == PackageManager.PERMISSION_GRANTED;
        }
        if (granted) grantWebPermission(token); else denyPermission(token);
    }

    public static void onActivityPause() {
        runOnUiThread(new Runnable() { @Override public void run() {
            synchronized (LOCK) { for (Instance i : INSTANCES.values()) i.webView.onPause(); }
        }});
    }
    public static void onActivityResume() {
        runOnUiThread(new Runnable() { @Override public void run() {
            synchronized (LOCK) { for (Instance i : INSTANCES.values()) i.webView.onResume(); }
        }});
    }
    public static void onActivityDestroyed() {
        runOnUiThread(new Runnable() { @Override public void run() {
            // Do not retain an Activity or View tree across Activity teardown.
            synchronized (LOCK) {
                for (Instance i : INSTANCES.values()) i.destroy();
                INSTANCES.clear();
                RESERVED_IDS.clear();
                PENDING_PERMISSIONS.clear();
                RUNTIME_REQUEST_TO_TOKEN.clear();
                if (overlayRoot != null && overlayRoot.getParent() instanceof ViewGroup) {
                    ((ViewGroup) overlayRoot.getParent()).removeView(overlayRoot);
                }
                overlayRoot = null;
                activity = null;
            }
        }});
    }
    public static boolean handleBackPressed() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            final boolean[] result = new boolean[] { false };
            final CountDownLatch latch = new CountDownLatch(1);
            runOnUiThread(new Runnable() { @Override public void run() { result[0] = handleBackPressedOnUiThread(); latch.countDown(); } });
            try { latch.await(500, TimeUnit.MILLISECONDS); } catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
            return result[0];
        }
        return handleBackPressedOnUiThread();
    }
    private static boolean handleBackPressedOnUiThread() {
        synchronized (LOCK) {
            for (Instance i : INSTANCES.values()) {
                if (i.fullscreenView != null) {
                    i.hideCustomView();
                    return true;
                }
            }
        }
        return false;
    }
    public static float getDisplayDensity() {
        Activity host = activity;
        if (host == null || host.getResources() == null || host.getResources().getDisplayMetrics() == null) return 1.0f;
        return host.getResources().getDisplayMetrics().density;
    }

    private static ArrayList<String> runtimePermissionsFor(String[] resources) {
        ArrayList<String> result = new ArrayList<String>();
        if (resources == null) return result;
        for (String resource : resources) {
            if (PermissionRequest.RESOURCE_VIDEO_CAPTURE.equals(resource) && !result.contains(Manifest.permission.CAMERA)) result.add(Manifest.permission.CAMERA);
            if (PermissionRequest.RESOURCE_AUDIO_CAPTURE.equals(resource) && !result.contains(Manifest.permission.RECORD_AUDIO)) result.add(Manifest.permission.RECORD_AUDIO);
            if (PermissionRequest.RESOURCE_PROTECTED_MEDIA_ID.equals(resource)) { /* DRM does not require a runtime permission. */ }
        }
        return result;
    }

    private static final class PendingPermission {
        final int instanceId;
        final PermissionRequest request;
        final GeolocationPermissions.Callback geolocationCallback;
        final String geolocationOrigin;
        PendingPermission(int id, PermissionRequest value) {
            instanceId = id;
            request = value;
            geolocationCallback = null;
            geolocationOrigin = "";
        }
        PendingPermission(int id, String origin, GeolocationPermissions.Callback callback) {
            instanceId = id;
            request = null;
            geolocationCallback = callback;
            geolocationOrigin = origin == null ? "" : origin;
        }
    }

    private static final class Instance {
        final int id;
        final WebView webView;
        final Activity host;
        boolean mediaAutoplay;
        boolean muted;
        boolean transparent;
        boolean allowMixedContent;
        int newWindowPolicy = NEW_WINDOW_SAME_VIEW;
        final Map<WebView, Runnable> pendingPopups = new HashMap<WebView, Runnable>();
        View fullscreenView;
        WebChromeClient.CustomViewCallback fullscreenCallback;
        int previousSystemUiVisibility;
        final FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(1, 1);

        Instance(int instanceId, Activity activity, boolean enableJavaScript, boolean autoplay,
                 boolean isMuted, boolean transparentBackground, boolean allowMixed, String userAgent) {
            id = instanceId;
            host = activity;
            mediaAutoplay = autoplay;
            muted = isMuted;
            transparent = transparentBackground;
            allowMixedContent = allowMixed;
            // Use the Activity context so input controls/IME and fullscreen
            // custom views are attached to the same window as the UE surface.
            webView = new WebView(activity);
            webView.setTag("VeloWebView-" + id);
            configure(enableJavaScript, autoplay, isMuted, transparentBackground, allowMixed, userAgent);
        }

        FrameLayout.LayoutParams layoutParams() {
            params.gravity = Gravity.TOP | Gravity.START;
            return params;
        }

        void configure(boolean enableJavaScript, boolean autoplay, boolean isMuted,
                       boolean transparentBackground, boolean allowMixed, String userAgent) {
            WebSettings settings = webView.getSettings();
            settings.setJavaScriptEnabled(enableJavaScript);
            settings.setDomStorageEnabled(true);
            settings.setDatabaseEnabled(true);
            settings.setMediaPlaybackRequiresUserGesture(!autoplay);
            settings.setSupportZoom(false);
            settings.setSupportMultipleWindows(true);
            // User-initiated window.open() works; background scripts cannot
            // create unsolicited windows.
            settings.setJavaScriptCanOpenWindowsAutomatically(false);
            settings.setBuiltInZoomControls(false);
            settings.setDisplayZoomControls(false);
            settings.setAllowFileAccess(true);
            settings.setAllowContentAccess(true);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                settings.setMixedContentMode(allowMixed ? WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE : WebSettings.MIXED_CONTENT_NEVER_ALLOW);
            }
            if (userAgent != null && userAgent.length() > 0) settings.setUserAgentString(userAgent);
            webView.setLayerType(View.LAYER_TYPE_HARDWARE, null);
            setBackgroundTransparent(transparentBackground);
            webView.addJavascriptInterface(new VeloJavascriptBridge(id), "VeloWebView");
            webView.setWebViewClient(new VeloWebViewClient(this));
            webView.setWebChromeClient(new VeloWebChromeClient(this));
            webView.setDownloadListener(new DownloadListener() {
                @Override public void onDownloadStart(String url, String userAgent, String contentDisposition, String mimetype, long contentLength) {
                    String filename = URLUtil.guessFileName(url, contentDisposition, mimetype);
                    notifyDownload(id, url, filename, mimetype == null ? "" : mimetype);
                }
            });
        }

        void setBounds(float x, float y, float width, float height, boolean inDp) {
            float density = inDp ? getDisplayDensity() : 1.0f;
            params.width = Math.max(1, Math.round(width * density));
            params.height = Math.max(1, Math.round(height * density));
            webView.setLayoutParams(params);
            webView.setX(x * density);
            webView.setY(y * density);
        }
        void setJavaScriptEnabled(boolean enabled) { webView.getSettings().setJavaScriptEnabled(enabled); }
        void setMediaAutoplay(boolean enabled) { mediaAutoplay = enabled; webView.getSettings().setMediaPlaybackRequiresUserGesture(!enabled); }
        void setMuted(boolean value) { muted = value; injectMutedState(); }
        void setUserAgent(String value) { webView.getSettings().setUserAgentString(value == null ? WebSettings.getDefaultUserAgent(host) : value); }
        void setBackgroundTransparent(boolean value) { transparent = value; webView.setBackgroundColor(value ? Color.TRANSPARENT : Color.WHITE); webView.setFocusable(true); }
        void setAllowMixedContent(boolean value) { allowMixedContent = value; if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) webView.getSettings().setMixedContentMode(value ? WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE : WebSettings.MIXED_CONTENT_NEVER_ALLOW); }
        void injectMutedState() {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT && muted) {
                webView.evaluateJavascript("(function(){var v=document.querySelectorAll('video,audio');for(var i=0;i<v.length;i++){v[i].muted=true;}})();", null);
            }
        }
        void detachFromParent() { if (webView.getParent() instanceof ViewGroup) ((ViewGroup) webView.getParent()).removeView(webView); }
        void destroy() {
            if (fullscreenView != null) hideCustomView();
            for (WebView popup : new ArrayList<WebView>(pendingPopups.keySet())) closePopup(popup);
            detachFromParent();
            webView.stopLoading();
            webView.loadUrl("about:blank");
            webView.clearHistory();
            webView.removeJavascriptInterface("VeloWebView");
            webView.setWebChromeClient(null);
            webView.setWebViewClient(null);
            webView.destroy();
        }
        boolean createPopup(final boolean userGesture, Message resultMsg) {
            if (newWindowPolicy == NEW_WINDOW_BLOCK) {
                // Android does not provide the target URL until a child WebView
                // is created. Reject synchronously so blocked pages cannot load.
                notifyNewWindowRequested(id, "", userGesture);
                return false;
            }
            if (resultMsg == null || !(resultMsg.obj instanceof WebView.WebViewTransport) || overlayRoot == null) return false;
            final WebView popup = new WebView(host);
            popup.getSettings().setJavaScriptEnabled(webView.getSettings().getJavaScriptEnabled());
            popup.getSettings().setDomStorageEnabled(true);
            popup.getSettings().setUserAgentString(webView.getSettings().getUserAgentString());
            final boolean[] resolved = new boolean[] { false };
            popup.setWebViewClient(new WebViewClient() {
                private boolean capture(String url) {
                    if (resolved[0]) return true;
                    if (url == null || url.length() == 0) return false;
                    String lower = url.toLowerCase(Locale.US);
                    if (lower.startsWith("about:blank") || lower.startsWith("javascript:")) return false;
                    resolved[0] = true;
                    final String targetURL = url;
                    MAIN_HANDLER.post(new Runnable() { @Override public void run() {
                        if (pendingPopups.containsKey(popup)) handlePopupURL(targetURL, userGesture);
                        closePopup(popup);
                    }});
                    return true;
                }
                @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                    return request != null && request.isForMainFrame() && capture(request.getUrl() == null ? "" : request.getUrl().toString());
                }
                @Override public boolean shouldOverrideUrlLoading(WebView view, String url) { return capture(url); }
                @Override public void onPageStarted(WebView view, String url, android.graphics.Bitmap favicon) { capture(url); }
            });
            final Runnable timeout = new Runnable() { @Override public void run() { closePopup(popup); } };
            pendingPopups.put(popup, timeout);
            popup.setVisibility(View.INVISIBLE);
            overlayRoot.addView(popup, new FrameLayout.LayoutParams(1, 1));
            ((WebView.WebViewTransport) resultMsg.obj).setWebView(popup);
            resultMsg.sendToTarget();
            // A blank popup that never receives a URL must not retain its View.
            MAIN_HANDLER.postDelayed(timeout, 30000);
            return true;
        }
        void handlePopupURL(String url, boolean userGesture) {
            notifyNewWindowRequested(id, url, userGesture);
            if (getInstance(id) != this) return;
            if (newWindowPolicy == NEW_WINDOW_SAME_VIEW) {
                String scheme = Uri.parse(url).getScheme();
                if (scheme != null && (scheme.equalsIgnoreCase("http") || scheme.equalsIgnoreCase("https")
                        || scheme.equalsIgnoreCase("about") || scheme.equalsIgnoreCase("data"))) {
                    webView.loadUrl(url);
                } else {
                    openExternal(url);
                }
            } else if (newWindowPolicy == NEW_WINDOW_EXTERNAL) {
                openExternal(url);
            } else if (newWindowPolicy == NEW_WINDOW_NOTIFY_ONLY || newWindowPolicy == NEW_WINDOW_BLOCK) {
                // The notification above is the only result for these policies.
            }
        }
        void openExternal(String url) {
            try {
                Uri uri = Uri.parse(url);
                String scheme = uri.getScheme();
                if (scheme == null || scheme.equalsIgnoreCase("javascript") || scheme.equalsIgnoreCase("file") || scheme.equalsIgnoreCase("content")) return;
                host.startActivity(new Intent(Intent.ACTION_VIEW, uri));
            } catch (Exception ignored) { }
        }
        void closePopup(WebView popup) {
            Runnable timeout = pendingPopups.remove(popup);
            if (timeout == null) return;
            MAIN_HANDLER.removeCallbacks(timeout);
            popup.stopLoading();
            popup.setWebViewClient(null);
            if (popup.getParent() instanceof ViewGroup) ((ViewGroup) popup.getParent()).removeView(popup);
            popup.destroy();
        }
        void showCustomView(View view, WebChromeClient.CustomViewCallback callback) {
            if (fullscreenView != null) { hideCustomView(); }
            fullscreenView = view;
            fullscreenCallback = callback;
            Window window = host.getWindow();
            View decor = window == null ? null : window.getDecorView();
            previousSystemUiVisibility = decor == null ? 0 : decor.getSystemUiVisibility();
            webView.setVisibility(View.INVISIBLE);
            if (overlayRoot != null) {
                overlayRoot.addView(view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
                view.setBackgroundColor(Color.BLACK);
            }
            if (decor != null && Build.VERSION.SDK_INT >= 19) {
                decor.setSystemUiVisibility(previousSystemUiVisibility | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
            }
            notifyFullscreen(id, true);
        }
        void hideCustomView() {
            if (fullscreenView == null) return;
            if (fullscreenView.getParent() instanceof ViewGroup) ((ViewGroup) fullscreenView.getParent()).removeView(fullscreenView);
            if (fullscreenCallback != null) fullscreenCallback.onCustomViewHidden();
            fullscreenView = null;
            fullscreenCallback = null;
            webView.setVisibility(View.VISIBLE);
            Window window = host.getWindow();
            View decor = window == null ? null : window.getDecorView();
            if (decor != null) decor.setSystemUiVisibility(previousSystemUiVisibility);
            notifyFullscreen(id, false);
        }
    }

    private static final class VeloJavascriptBridge {
        private final int instanceId;
        VeloJavascriptBridge(int id) { instanceId = id; }
        @JavascriptInterface public void postMessage(String message) { notifyJavaScriptMessage(instanceId, message == null ? "" : message); }
    }

    private static final class VeloWebViewClient extends WebViewClient {
        private final Instance owner;
        VeloWebViewClient(Instance value) { owner = value; }
        @Override public void onPageStarted(WebView view, String url, android.graphics.Bitmap favicon) {
            notifyPageStarted(owner.id, url); notifyUrlChanged(owner.id, url);
        }
        @Override public void onPageFinished(WebView view, String url) {
            owner.injectMutedState(); notifyPageLoaded(owner.id, url); notifyUrlChanged(owner.id, url);
        }
        @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            String url = request == null || request.getUrl() == null ? "" : request.getUrl().toString();
            notifyBeforeNavigate(owner.id, url);
            if (url.startsWith("http://") || url.startsWith("https://") || url.startsWith("about:") || url.startsWith("data:")) return false;
            try { Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url)); hostStartActivity(intent); } catch (Exception ignored) { }
            return true;
        }
        @Override public boolean shouldOverrideUrlLoading(WebView view, String url) {
            notifyBeforeNavigate(owner.id, url);
            if (url == null || url.startsWith("http://") || url.startsWith("https://") || url.startsWith("about:") || url.startsWith("data:")) return false;
            try { hostStartActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))); } catch (Exception ignored) { }
            return true;
        }
        private void hostStartActivity(Intent intent) { if (activity != null) activity.startActivity(intent); }
        @Override public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
            String url = request == null || request.getUrl() == null ? "" : request.getUrl().toString();
            int code = error == null ? -1 : error.getErrorCode(); String desc = error == null ? "WebView error" : String.valueOf(error.getDescription());
            notifyPageError(owner.id, code, desc, url);
        }
        @Override public void onReceivedError(WebView view, int errorCode, String description, String failingUrl) { notifyPageError(owner.id, errorCode, description, failingUrl); }
        @Override public void onReceivedHttpError(WebView view, WebResourceRequest request, WebResourceResponse errorResponse) {
            if (request != null && request.isForMainFrame() && errorResponse != null) notifyPageError(owner.id, errorResponse.getStatusCode(), errorResponse.getReasonPhrase(), request.getUrl().toString());
        }
        @Override public void onReceivedSslError(WebView view, SslErrorHandler handler, SslError error) {
            // Secure-by-default: never ignore certificate errors.
            if (handler != null) handler.cancel();
            notifyPageError(owner.id, -2, "SSL certificate error", error == null ? "" : error.getUrl());
        }
        @Override public void doUpdateVisitedHistory(WebView view, String url, boolean isReload) { notifyUrlChanged(owner.id, url); }
    }

    private static final class VeloWebChromeClient extends WebChromeClient {
        private final Instance owner;
        VeloWebChromeClient(Instance value) { owner = value; }
        @Override public void onReceivedTitle(WebView view, String title) { notifyTitleChanged(owner.id, title); }
        @Override public boolean onConsoleMessage(android.webkit.ConsoleMessage consoleMessage) {
            if (consoleMessage != null) notifyConsole(owner.id, consoleMessage.message(), consoleMessage.sourceId(), consoleMessage.lineNumber());
            return true;
        }
        @Override public boolean onCreateWindow(WebView view, boolean isDialog, boolean isUserGesture, Message resultMsg) {
            return owner.createPopup(isUserGesture, resultMsg);
        }
        @Override public void onCloseWindow(WebView window) { owner.closePopup(window); }
        @Override public void onPermissionRequest(final PermissionRequest request) {
            if (request == null) return;
            runOnUiThread(new Runnable() { @Override public void run() {
                int token = NEXT_PERMISSION_TOKEN.getAndIncrement();
                synchronized (LOCK) { PENDING_PERMISSIONS.put(token, new PendingPermission(owner.id, request)); }
                StringBuilder resources = new StringBuilder();
                String[] values = request.getResources();
                if (values != null) for (int i = 0; i < values.length; ++i) { if (i > 0) resources.append(','); resources.append(values[i]); }
                notifyPermission(owner.id, token, resources.toString(), request.getOrigin() == null ? "" : request.getOrigin().toString());
            }});
        }
        @Override public void onGeolocationPermissionsShowPrompt(final String origin, final GeolocationPermissions.Callback callback) {
            if (callback == null) return;
            runOnUiThread(new Runnable() { @Override public void run() {
                int token = NEXT_PERMISSION_TOKEN.getAndIncrement();
                synchronized (LOCK) { PENDING_PERMISSIONS.put(token, new PendingPermission(owner.id, origin, callback)); }
                notifyPermission(owner.id, token, "GEOLOCATION", origin == null ? "" : origin);
            }});
        }
        @Override public void onShowCustomView(View view, CustomViewCallback callback) { owner.showCustomView(view, callback); }
        @Override public void onShowCustomView(View view, int requestedOrientation, CustomViewCallback callback) { owner.showCustomView(view, callback); }
        @Override public void onHideCustomView() { owner.hideCustomView(); }
    }

    // Native callback wrappers.  Calling a Java native method can throw
    // UnsatisfiedLinkError when an app strips the optional Android module;
    // mark it unavailable and keep WebView usable in that case.
    private static void notifyPageStarted(int id, String url) { if (nativeUnavailable) return; try { nativeOnPageStarted(id, url == null ? "" : url); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }
    private static void notifyPageLoaded(int id, String url) { if (nativeUnavailable) return; try { nativeOnPageLoaded(id, url == null ? "" : url); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }
    private static void notifyPageError(int id, int code, String description, String url) { if (nativeUnavailable) return; try { nativeOnPageError(id, code, description == null ? "" : description, url == null ? "" : url); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }
    private static void notifyUrlChanged(int id, String url) { if (nativeUnavailable) return; try { nativeOnURLChanged(id, url == null ? "" : url); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }
    private static void notifyTitleChanged(int id, String title) { if (nativeUnavailable) return; try { nativeOnTitleChanged(id, title == null ? "" : title); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }
    private static void notifyJavaScriptMessage(int id, String message) { if (nativeUnavailable) return; try { nativeOnJavaScriptMessage(id, message == null ? "" : message); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }
    private static void notifyConsole(int id, String message, String source, int line) { if (nativeUnavailable) return; try { nativeOnConsoleMessage(id, message == null ? "" : message, source == null ? "" : source, line); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }
    private static void notifyDownload(int id, String url, String filename, String mime) { if (nativeUnavailable) return; try { nativeOnDownloadRequested(id, url == null ? "" : url, filename == null ? "" : filename, mime == null ? "" : mime); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }
    private static void notifyPermission(int id, int token, String resources, String origin) { if (nativeUnavailable) { denyPermission(token); return; } try { nativeOnPermissionRequested(id, token, resources == null ? "" : resources, origin == null ? "" : origin); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; denyPermission(token); } }
    private static void notifyFullscreen(int id, boolean fullscreen) { if (nativeUnavailable) return; try { nativeOnFullscreenChanged(id, fullscreen); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }
    private static void notifyBeforeNavigate(int id, String url) { if (nativeUnavailable) return; try { nativeOnBeforeNavigate(id, url == null ? "" : url); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }
    private static void notifyNewWindowRequested(int id, String url, boolean userGesture) { if (nativeUnavailable) return; try { nativeOnNewWindowRequested(id, url == null ? "" : url, userGesture); } catch (UnsatisfiedLinkError e) { nativeUnavailable = true; } }

    private static native void nativeOnPageStarted(int id, String url);
    private static native void nativeOnPageLoaded(int id, String url);
    private static native void nativeOnPageError(int id, int code, String description, String url);
    private static native void nativeOnURLChanged(int id, String url);
    private static native void nativeOnTitleChanged(int id, String title);
    private static native void nativeOnJavaScriptMessage(int id, String message);
    private static native void nativeOnConsoleMessage(int id, String message, String source, int line);
    private static native void nativeOnDownloadRequested(int id, String url, String filename, String mime);
    private static native void nativeOnPermissionRequested(int id, int token, String resources, String origin);
    private static native void nativeOnFullscreenChanged(int id, boolean fullscreen);
    private static native void nativeOnBeforeNavigate(int id, String url);
    private static native void nativeOnNewWindowRequested(int id, String url, boolean userGesture);
}
