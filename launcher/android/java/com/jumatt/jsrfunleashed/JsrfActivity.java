package com.jumatt.jsrfunleashed;

import android.app.AlertDialog;
import android.content.pm.ActivityInfo;
import android.hardware.input.InputManager;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Build;
import android.graphics.Color;
import android.util.Log;
import android.view.InputDevice;
import android.view.Display;
import android.view.WindowManager;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.widget.RelativeLayout;
import android.widget.Toast;

import java.io.File;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL's activity, which loads libSDL2.so and libmain.so (the
 * engine) and calls its SDL_main with the folders below
 * (port/android/jsrf_android.c reads them). On top of SDL's surface, the
 * on-screen controls (TouchPadView) while no controller is connected, and
 * the settings button.
 */
public class JsrfActivity extends SDLActivity
        implements TouchPadView.Listener, InputManager.InputDeviceListener, DisplayManager.DisplayListener {

    /* port/android/jsrf_android.c */
    static native void nativeTouch(int buttons, int analog, int x, int y, boolean stick, boolean shown);
    static native void nativeVulkanSurface(Surface surface);
    static native boolean nativeWidescreen();
    static native void nativeSetWidescreen(boolean on);
static native int nativeRstickCamera();            /* 0 off, 1 the camera, 2 tracking */
    static native void nativeSetRstickCamera(int mode);
    static native void nativePhotoMode();
    static native String[] nativeMapNames();          /* the map selector (jsrf_maps.h) */
    static native void nativeGoToMap(int index);
    static native void nativeSetPerformanceOverlay(boolean on);
    static native String nativePerformanceStats();
    static native void nativeSetControllerMapping(boolean swapTriggers, boolean nintendo);
    static native String nativeVersion();
    static native void nativeLog(String line);

    private TouchPadView mPad;
    private InputManager mInput;
    private String mPadsSaid = "";
    private boolean mSwapTriggers, mNintendo;
    private boolean mPerformanceOverlay;
    private boolean mReleaseBuild;
    private DisplayManager mDisplays;
    private SecondaryMapPresentation mMapDisplay;
    private boolean mResumed;
    private final SurfaceHolder.Callback mVulkanSurfaceCallback = new SurfaceHolder.Callback() {
        @Override public void surfaceCreated(SurfaceHolder holder) { publishVulkanSurface(holder); }
        @Override public void surfaceChanged(SurfaceHolder holder,int format,int width,int height) { publishVulkanSurface(holder); }
        @Override public void surfaceDestroyed(SurfaceHolder holder) { nativeVulkanSurface(null); }
    };
    private void publishVulkanSurface(SurfaceHolder holder) {
        Surface surface=holder.getSurface();
        nativeVulkanSurface(mResumed && surface!=null && surface.isValid() ? surface : null);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        File data = getExternalFilesDir(null);
        return new String[] {
            new File(data, "game").getAbsolutePath(),   /* default.xbe and Media/ */
            data.getAbsolutePath(),                      /* Logs/, settings, save/ */
            getCacheDir().getAbsolutePath(),             /* temporary files */
        };
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (mLayout == null) return;        /* SDL could not load the engine: it says so */
        restoreFullscreen();
        if (mSurface != null) mSurface.getHolder().addCallback(mVulkanSurfaceCallback);
        mSwapTriggers = getSharedPreferences("controller", MODE_PRIVATE).getBoolean("swap_triggers", false);
        mNintendo = getSharedPreferences("controller", MODE_PRIVATE).getBoolean("nintendo", false);
        mReleaseBuild=getResources().getBoolean(R.bool.cn_release_build);
        mPerformanceOverlay = !mReleaseBuild && Settings.get(this, "performance_overlay", 0) != 0;
        nativeSetPerformanceOverlay(mPerformanceOverlay);
        nativeSetControllerMapping(mSwapTriggers, mNintendo);
        mPad = new TouchPadView(this, this);
        mPad.setPerformanceOverlay(mPerformanceOverlay);
        mLayout.addView(mPad, new RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        mInput = (InputManager) getSystemService(INPUT_SERVICE);
        if (mInput != null) mInput.registerInputDeviceListener(this, null);
        updateControls();
        mDisplays = (DisplayManager)getSystemService(DISPLAY_SERVICE);
        if (mDisplays != null) mDisplays.registerDisplayListener(this, null);
    }

    @Override protected void onResume() {
        super.onResume();
        mResumed = true;
        if (mLayout != null) restoreFullscreen();
        if (mSurface != null) publishVulkanSurface(mSurface.getHolder());
        updateMapDisplay();
    }

    @Override
    protected void onPause() {
        mResumed = false;
        if (mLayout != null) nativeVulkanSurface(null);
        closeMapDisplay();
        if (mPad != null) mPad.reset();
        super.onPause();
    }

    /* Returning from Home can leave a status-bar color view covering the top
     * 55 pixels even after SDL's surface has grown back to full size. Restore
     * both the layout policy and bar visibility when this window owns focus. */
    private void restoreFullscreen() {
        Window window=getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        window.clearFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);
        window.setStatusBarColor(Color.TRANSPARENT);
        window.setNavigationBarColor(Color.TRANSPARENT);
        window.setStatusBarContrastEnforced(false);
        window.setNavigationBarContrastEnforced(false);
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController bars=window.getInsetsController();
            if (bars!=null) {
                bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                bars.hide(WindowInsets.Type.systemBars());
            }
        } else {
            window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    |View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    |View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    |View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused && mLayout!=null) restoreFullscreen();
    }

    @Override
    protected void onDestroy() {
        if (mSurface != null) mSurface.getHolder().removeCallback(mVulkanSurfaceCallback);
        closeMapDisplay();
        if (mDisplays != null) mDisplays.unregisterDisplayListener(this);
        if (mInput != null) mInput.unregisterInputDeviceListener(this);
        super.onDestroy();
    }

    private void closeMapDisplay() {
        if (mMapDisplay != null) {
            mMapDisplay.dismiss();
            mMapDisplay = null;
        }
    }

    private void updateMapDisplay() {
        if (!mResumed || mLayout == null || mDisplays == null || Settings.get(this,"secondary_map",1)==0) {
            closeMapDisplay(); return;
        }
        int primary = getWindowManager().getDefaultDisplay().getDisplayId();
        Display target = null;
        for (Display display : mDisplays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (display.getDisplayId()!=primary && display.getState()!=Display.STATE_OFF) { target=display; break; }
        }
        if (target==null) { closeMapDisplay(); return; }
        if (mMapDisplay!=null && mMapDisplay.getDisplay().getDisplayId()==target.getDisplayId() && mMapDisplay.isShowing()) return;
        closeMapDisplay();
        SecondaryMapPresentation next = new SecondaryMapPresentation(this,target,new File(getExternalFilesDir(null),"game"));
        try {
            next.show(); mMapDisplay=next;
            Log.i("JSRF-Map","Companion map on display "+target.getDisplayId());
        } catch (WindowManager.InvalidDisplayException | SecurityException ex) {
            Log.w("JSRF-Map","Secondary display unavailable",ex);
            next.dismiss();
        }
    }
    @Override public void onDisplayAdded(int id) { updateMapDisplay(); }
    @Override public void onDisplayRemoved(int id) { updateMapDisplay(); }
    @Override public void onDisplayChanged(int id) { updateMapDisplay(); }

    /** Landscape, either way up. SDL would let a resizable window turn with
     *  the phone, into portrait too. */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    /* ---- controllers and the on-screen controls ---- */

    @Override public void onInputDeviceAdded(int id)   { updateControls(); }
    @Override public void onInputDeviceRemoved(int id) { updateControls(); }
    @Override public void onInputDeviceChanged(int id) { updateControls(); }

    /** Android calls it a gamepad or a joystick. */
    private static boolean looksLikePad(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    /** Its sticks' and triggers' axes, and its d-pad's when it reports one as
     *  axes: {axes, hat axes}, counted as SDL counts them. */
    private static int[] axesOf(InputDevice d) {
        int axes = 0, hats = 0;
        for (InputDevice.MotionRange r : d.getMotionRanges()) {
            if ((r.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) == 0) continue;
            if (r.getAxis() == MotionEvent.AXIS_HAT_X || r.getAxis() == MotionEvent.AXIS_HAT_Y) hats++;
            else axes++;
        }
        return new int[] { axes, hats };
    }

    /** A controller: a gamepad with a stick or a d-pad that reports as axes.
     *
     *  Being called a gamepad is not enough. Phones list some of their own
     *  parts as gamepads -- the fingerprint reader's or the side buttons'
     *  device declares gamepad buttons -- and counting those hid the on-screen
     *  controls on such a phone with nothing connected. They have no axes.
     *  SDL is told the same (SDL_TV_REMOTE_AS_JOYSTICK in
     *  port/android/jsrf_android.c), so the engine does not take one for
     *  player 1's controller either. */
    private static boolean isController(InputDevice d) {
        if (!looksLikePad(d)) return false;
        int[] a = axesOf(d);
        return a[0] >= 2 || a[1] >= 2;
    }

    private boolean touchPref() {
        return Settings.get(this, "touch", 1) != 0;
    }

    /** The on-screen controls while no controller is connected (and the
     *  player has not turned them off). */
    private void updateControls() {
        if (mPad == null) return;
        boolean pad = false;
        StringBuilder said = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (!looksLikePad(d)) continue;
            boolean c = isController(d);
            int[] a = axesOf(d);
            if (c) pad = true;
            /* Into the log: what each device Android calls a gamepad was
             * taken for. Its name and counts only -- a descriptor can carry
             * a Bluetooth address, and logs end up in public issues. */
            said.append(String.format(java.util.Locale.ROOT, "[PAD] Android: %s: %d axes, %d hat axes -- %s\n",
                    d.getName(), a[0], a[1], c ? "a controller" : "no stick or d-pad, not a controller"));
        }
        boolean want = !pad && touchPref();
        mPad.setControls(want);
        said.append("[PAD] Android: on-screen controls ").append(pad ? "hidden (a controller)" : want ? "shown" : "hidden (the setting)");
        String line = said.toString();
        if (!line.equals(mPadsSaid)) {
            mPadsSaid = line;
            Log.i("JSRF", line);
            for (String l : line.split("\n")) nativeLog(l);
        }
    }

    /* ---- settings ---- */

    private AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
    }

    @Override
    public void onSettings() {
        final boolean wide = nativeWidescreen();
        final boolean touch = touchPref();
final int rcam = nativeRstickCamera();
        final boolean perf = mPerformanceOverlay;
        final int scale = Settings.get(this, "scale", 2);
        String[] items = {
            "Widescreen: " + (wide ? "on" : "off"),
            "On-screen controls: " + (touch ? "shown" : "hidden"),
            "Resolution: " + scale + "x",
            "Camera on the right stick: " + (rcam == 2 ? "tracking" : rcam != 0 ? "on" : "off"),
            "肩键/扳机互换 (LB↔LT、RB↔RT)：" + (mSwapTriggers ? "开" : "关"),
            "ABXY 布局：" + (mNintendo ? "任天堂" : "微软"),
            "性能叠层：" + (perf ? "开" : "关"),
            "Photo mode",
            "Go to a map",
            "副屏地图：" + (Settings.get(this,"secondary_map",1)!=0 ? "开" : "关"),
            "切换角色",
            "Controls",
        };
        // Retain action IDs while excluding development tools from release UI.
        final int[] actions=mReleaseBuild ? new int[] {0,1,2,3,4,5,7,8,9,10,11}
                : new int[] {0,1,2,3,4,5,6,7,8,9,10,11};
        String[] visibleItems=new String[actions.length];
        for (int i=0;i<actions.length;i++) visibleItems[i]=items[actions[i]];
        mPad.reset();
        dialog().setTitle("JSRF Unleashed " + nativeVersion())
            .setItems(visibleItems, (d, which) -> {
                switch (actions[which]) {
                case 0:
                    nativeSetWidescreen(!wide);
                    break;
                case 1:
                    Settings.set(this, "touch", touch ? 0 : 1);
                    updateControls();
                    break;
                case 2:
                    pickScale(scale);
                    break;
                case 3:
                    pickRstickCamera(rcam);
                    break;
                case 4:
                    setControllerMapping(!mSwapTriggers, mNintendo);
                    break;
                case 5:
                    pickControllerLayout();
                    break;
                case 6:
                    mPerformanceOverlay = !perf;
                    Settings.set(this, "performance_overlay", mPerformanceOverlay ? 1 : 0);
                    nativeSetPerformanceOverlay(mPerformanceOverlay);
                    mPad.setPerformanceOverlay(mPerformanceOverlay);
                    break;
                case 7:
                    nativePhotoMode();
                    break;
                case 8:
                    pickMap();
                    break;
                case 9:
                    Settings.set(this,"secondary_map",Settings.get(this,"secondary_map",1)!=0 ? 0 : 1);
                    updateMapDisplay();
                    break;
                case 10:
                    AlertDialog characters=dialog().setTitle("切换角色")
                        .setNegativeButton("关闭",null).create();
                    characters.setView(new CharacterSwitchBar(this,characters::dismiss));
                    characters.show();
                    break;
                default:
                    showControls();
                    break;
                }
            })
            .setNegativeButton("Close", null)
            .show();
    }

    /* The map selector: straight to a map, to skate it freely. */
    private void pickMap() {
        String[] items = nativeMapNames();
        if (items == null) return;
        dialog().setTitle("Go to a map")
            .setItems(items, (d, which) -> nativeGoToMap(which))
            .setNegativeButton("Cancel", null)
            .show();
    }

    /* The camera on the right stick: on (the skater goes where it looks),
     * tracking (it only looks around: the skater keeps his heading), or off
     * (the game's own camera). */
    private void pickRstickCamera(int now) {
        String[] names = { "Off (the game's camera)", "On", "Tracking (look around, keep going)" };
        String[] items = new String[3];
        int[] modes = { 1, 2, 0 };
        for (int i = 0; i < 3; i++)
            items[i] = names[modes[i]] + (modes[i] == now ? "  ✓" : "");
        dialog().setTitle("Camera on the right stick")
            .setItems(items, (d, which) -> nativeSetRstickCamera(modes[which]))
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void pickScale(int now) {
        String[] items = new String[4];
        for (int s = 1; s <= 4; s++)
            items[s - 1] = s + "x (" + 640 * s + " x " + 480 * s + ")" + (s == now ? "  ✓" : "");
        dialog().setTitle("Resolution")
            .setItems(items, (d, which) -> {
                Settings.set(this, "scale", which + 1);
                if (which + 1 != now)
                    Toast.makeText(this, "Used the next time the game starts.", Toast.LENGTH_LONG).show();
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void setControllerMapping(boolean swapTriggers, boolean nintendo) {
        boolean saved = getSharedPreferences("controller", MODE_PRIVATE).edit()
            .putBoolean("swap_triggers", swapTriggers)
            .putBoolean("nintendo", nintendo).commit();
        if (!saved) {
            Toast.makeText(this, "无法保存手柄设置，请重试。", Toast.LENGTH_LONG).show();
            return;
        }
        mSwapTriggers = swapTriggers;
        mNintendo = nintendo;
        nativeSetControllerMapping(mSwapTriggers, mNintendo);
        Toast.makeText(this, "手柄设置已保存，立即生效。", Toast.LENGTH_SHORT).show();
    }

    private void pickControllerLayout() {
        dialog().setTitle("实体手柄 ABXY 布局")
            .setSingleChoiceItems(new String[] { "微软（默认）", "任天堂（A↔B、X↔Y）" },
                mNintendo ? 1 : 0, (d, which) -> {
                    setControllerMapping(mSwapTriggers, which == 1);
                    d.dismiss();
                })
            .setNegativeButton("取消", null)
            .show();
    }

    private void showControls() {
        dialog().setTitle("Controls")
            .setMessage("A controller works as on the console (Xbox, PlayStation, Switch Pro and "
                + "others, over Bluetooth or USB): A jump, R trigger talk and spray, Start, "
                + "Back/Select Back.\n\n"
                + "On screen: put your left thumb anywhere on the left half to steer; A B X Y are "
                + "the pad's, R and L its triggers, the hollow and filled circles White and Black; "
                + "Start and Back are at the top.\n\n"
                + "With the camera on the right stick (the settings), a controller's right stick "
                + "turns the camera around you and it eases back behind you when let go; "
                + "L trigger puts it straight behind at once. Set to tracking, it only looks "
                + "around: you keep going the way you were.\n\n"
                + "Photo mode (the settings, or hold Back on a controller): move the camera freely with "
                + "the sticks, the triggers lower and raise it, Y hides the HUD, A the help, B goes back "
                + "to the game.\n\n"
                + "A keyboard: WASD (by position) moves, arrows the camera, Space jump, E spray, "
                + "Enter Start, Esc Back.\n\n"
                + "A new resolution is used the next time the game starts.")
            .setPositiveButton("OK", null)
            .show();
    }
}
