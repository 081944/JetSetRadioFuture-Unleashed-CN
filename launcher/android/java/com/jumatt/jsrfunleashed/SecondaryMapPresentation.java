package com.jumatt.jsrfunleashed;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.io.File;

/** Non-focusable: lower-screen touches must not pause SDL on the upper screen
 * or steal the physical controller. Dismissal stops telemetry and frame ticks. */
final class SecondaryMapPresentation extends Presentation {
    private final File game;
    private final Handler handler=new Handler(Looper.getMainLooper());
    private SecondaryMapView map;
    private boolean running;
    private final Runnable tick=new Runnable() {
        @Override public void run() {
            if (!running) return;
            if (map!=null) map.requestRender();
            handler.postDelayed(this,33);
        }
    };
    SecondaryMapPresentation(Context context,Display display,File game) {
        super(context,display,android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        this.game=game;
        Window w=getWindow();
        if (w!=null) w.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE|WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        FrameLayout root=new FrameLayout(getContext());
        TextView message=new TextView(getContext());
        message.setTextColor(Color.WHITE); message.setTextSize(18); message.setGravity(Gravity.CENTER);
        float density=getContext().getResources().getDisplayMetrics().density;
        MapPauseCaption caption=new MapPauseCaption(getContext(),game);
        int inset=Math.round(12*density);
        caption.setVisibility(View.GONE);
        map=new SecondaryMapView(getContext(),game,new SecondaryMapView.Status() {
            @Override public void show(String text) {
                message.setText(text); message.setVisibility(text.isEmpty() ? View.GONE : View.VISIBLE);
            }
            @Override public void caption(String area,String target) {
                caption.setCaption(area,target);
            }
        });
        root.addView(map,new FrameLayout.LayoutParams(-1,-1));
        root.addView(message,new FrameLayout.LayoutParams(-1,-1));
        FrameLayout.LayoutParams heading=new FrameLayout.LayoutParams(-1,Math.round(120*density),Gravity.TOP|Gravity.LEFT);
        root.addView(caption,heading);
        FrameLayout.LayoutParams characters=new FrameLayout.LayoutParams(-2,-2,Gravity.BOTTOM|Gravity.RIGHT);
        int margin=Math.round(12*getContext().getResources().getDisplayMetrics().density);
        characters.setMargins(margin,margin,0,Math.round(4*density));
        root.addView(new CharacterSwitchBar(getContext(),null,true),characters);
        message.setText("进入游戏后显示地图");
        setContentView(root);
        Window w=getWindow();
        if (w!=null) w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                |View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                |View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }
    @Override protected void onStart() {
        super.onStart(); running=true;
        SecondaryMapView.nativeEnable(true);
        if (map!=null) map.onResume();
        handler.post(tick);
    }
    @Override protected void onStop() {
        running=false; handler.removeCallbacks(tick);
        SecondaryMapView.nativeEnable(false);
        if (map!=null) map.onPause();
        super.onStop();
    }
}
