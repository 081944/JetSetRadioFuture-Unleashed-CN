package com.jumatt.jsrfunleashed;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Matrix;
import android.graphics.Shader;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.util.Log;
import android.view.View;
import android.view.Choreographer;
import java.io.File;
import java.util.concurrent.Executors;

/** Original pause textures: animated goal noise and a left-aligned title plate. */
final class MapPauseCaption extends View {
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG|Paint.SUBPIXEL_TEXT_FLAG);
    private final RectF strip=new RectF();
    private BitmapShader noise,titleGrid;
    private final Matrix textureMatrix=new Matrix();
    private String area="",goal="";
    private long changed,frameTime;
    private boolean framePending;
    private final Choreographer.FrameCallback animation=time -> {
        framePending=false; frameTime=time/1000000L;
        invalidate();
        scheduleFrame();
    };
    private void scheduleFrame() {
        if (!framePending && isAttachedToWindow() && getWindowVisibility()==VISIBLE
                && getVisibility()==VISIBLE && !goal.isEmpty()) {
            framePending=true;
            Choreographer.getInstance().postFrameCallback(animation);
        }
    }
    private void stopFrames() {
        Choreographer.getInstance().removeFrameCallback(animation); framePending=false;
    }
    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); scheduleFrame(); }
    @Override protected void onDetachedFromWindow() { stopFrames(); super.onDetachedFromWindow(); }
    @Override protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        if (visibility==VISIBLE) scheduleFrame(); else stopFrames();
    }
    MapPauseCaption(Context context,File game) {
        super(context); setFocusable(false); setClickable(false);
        java.util.concurrent.ExecutorService loader=Executors.newSingleThreadExecutor();
        loader.execute(() -> {
            try {
                PauseBannerData.Assets assets=PauseBannerData.loadBoth(game);
                Bitmap image=Bitmap.createBitmap(assets.banner,512,40,Bitmap.Config.ARGB_8888);
                Bitmap grid=Bitmap.createBitmap(assets.title,PauseBannerData.TITLE_WIDTH,30,Bitmap.Config.ARGB_8888);
                post(() -> {
                    noise=new BitmapShader(image,Shader.TileMode.REPEAT,Shader.TileMode.REPEAT);
                    titleGrid=new BitmapShader(grid,Shader.TileMode.CLAMP,Shader.TileMode.CLAMP);
                    invalidate();
                });
            } catch (Exception ex) { Log.w("JSRF-Map","Cannot load original pause banner",ex); }
            finally { loader.shutdown(); }
        });
    }
    void setCaption(String area,String goal) {
        if (!this.goal.equals(goal)) changed=SystemClock.uptimeMillis();
        this.area=area; this.goal=goal;
        setVisibility(area.isEmpty() && goal.isEmpty() ? GONE : VISIBLE);
        if (goal.isEmpty()) stopFrames(); else scheduleFrame();
        invalidate();
    }
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float scale=getWidth()/640f;
        long now=frameTime!=0 ? Math.max(changed,frameTime) : SystemClock.uptimeMillis();
        paint.setTypeface(Typeface.DEFAULT_BOLD); paint.setTextSkewX(-.17f);
        paint.setShader(null); paint.setAlpha(255);
        if (!goal.isEmpty()) {
            strip.set(48*scale,48*scale,592*scale,80*scale);
            if (noise!=null) {
                // Jump to a fresh UV patch at 30 Hz, rather than sliding a
                // static bitmap. Reuse the game's original noise pixels.
                int phase=(int)(now/33)*1103515245+12345;
                textureMatrix.setScale(scale,scale);
                textureMatrix.postTranslate((phase&511)*scale,((phase>>>12)&31)*scale);
                noise.setLocalMatrix(textureMatrix); paint.setShader(noise);
            } else paint.setColor(0xD9797979);
            canvas.drawRect(strip,paint); paint.setShader(null);
            paint.setColor(0xFFFFFFFF); paint.setTextSize(18*scale);
            float width=paint.measureText(goal);
            // Every objective scrolls, including short ones. Keep the first
            // copy at the shared left inset, then wrap copies from the right.
            float period=Math.max(strip.width(),width+64*scale);
            float x=strip.left+8*scale-((now-changed)*.05f*scale)%period;
            canvas.save(); canvas.clipRect(strip);
            canvas.drawText(goal,x,71*scale,paint);
            canvas.drawText(goal,x+period,71*scale,paint); canvas.restore();
        }
        if (!area.isEmpty()) {
            float left=48*scale,top=96*scale,height=34*scale;
            paint.setTextSize(27*scale);
            float width=Math.max(320*scale,paint.measureText(area)+44*scale);
            if (titleGrid!=null) {
                textureMatrix.setScale(width/PauseBannerData.TITLE_WIDTH,height/30);
                textureMatrix.postTranslate(left,top); titleGrid.setLocalMatrix(textureMatrix);
                paint.setShader(titleGrid);
                canvas.drawRect(left,top,left+width,top+height,paint);
                paint.setShader(null);
            }
            paint.setColor(0xFF003A53); paint.setTextSize(27*scale);
            canvas.drawText(area,left+8*scale,124*scale,paint);
        }
    }
}
