package com.jumatt.jsrfunleashed;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.util.Log;
import android.view.View;
import java.io.File;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Circular glass button. Portrait pixels come from the user's game folder. */
final class CharacterBubble extends View {
    private static final ExecutorService loader=Executors.newSingleThreadExecutor();
    private static final ConcurrentHashMap<String,Bitmap> cache=new ConcurrentHashMap<>();
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final RectF area=new RectF();
    private final Matrix matrix=new Matrix();
    private final File game;
    private final int id;
    private Bitmap portrait;
    private boolean current,loading;
    CharacterBubble(Context context,File game,int id) {
        super(context); this.game=game; this.id=id;
        setClickable(true); setFocusable(false);
        setLayerType(LAYER_TYPE_SOFTWARE,null);
    }
    void setCurrent(boolean value) { current=value; invalidate(); }
    void loadPortrait() {
        if (portrait!=null || loading) return;
        String key=game.getAbsolutePath()+":"+id;
        portrait=cache.get(key);
        if (portrait!=null) { invalidate(); return; }
        loading=true;
        loader.execute(() -> {
            try {
                CharacterPortraitData data=CharacterPortraitData.load(game,id);
                Bitmap image=Bitmap.createBitmap(data.pixels,data.width,data.width,Bitmap.Config.ARGB_8888);
                cache.put(key,image);
                post(() -> { portrait=image; loading=false; invalidate(); });
            } catch (Exception ex) {
                Log.w("JSRF-Map","Cannot load character portrait "+id,ex);
                post(() -> { loading=false; invalidate(); });
            }
        });
    }
    @Override protected void drawableStateChanged() { super.drawableStateChanged(); invalidate(); }
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float d=Math.min(getWidth(),getHeight()),cx=getWidth()*.5f,cy=getHeight()*.5f;
        float radius=d*.43f,press=isPressed() ? .94f : 1;
        canvas.save(); canvas.scale(press,press,cx,cy);
        paint.setStyle(Paint.Style.FILL); paint.setShader(null);
        paint.setColor(current ? 0x9B64DAEC : 0x85405B70);
        paint.setShadowLayer(d*.08f,0,d*.045f,0x99051018);
        canvas.drawCircle(cx,cy,radius,paint); paint.clearShadowLayer();
        if (portrait!=null) {
            float size=radius*1.78f;
            BitmapShader shader=new BitmapShader(portrait,Shader.TileMode.CLAMP,Shader.TileMode.CLAMP);
            matrix.setScale(size/portrait.getWidth(),size/portrait.getHeight());
            matrix.postTranslate(cx-size*.5f,cy-size*.5f);
            shader.setLocalMatrix(matrix); paint.setShader(shader); paint.setAlpha(225);
            canvas.drawCircle(cx,cy,radius*.87f,paint); paint.setAlpha(255);
        }
        paint.setShader(new RadialGradient(cx-radius*.35f,cy-radius*.55f,radius*1.7f,
                new int[]{0x65FFFFFF,0x08C7F5FF,0x45102B43},new float[]{0,.5f,1},Shader.TileMode.CLAMP));
        canvas.drawCircle(cx,cy,radius,paint);
        paint.setShader(null); paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(current ? d*.035f : d*.018f);
        paint.setColor(current ? 0xEEA2F5FF : 0x99D3ECF5);
        canvas.drawCircle(cx,cy,radius,paint);
        paint.setStrokeWidth(d*.018f); paint.setColor(0xB3FFFFFF);
        area.set(cx-radius*.89f,cy-radius*.89f,cx+radius*.89f,cy+radius*.89f);
        canvas.drawArc(area,205,105,false,paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setShader(new LinearGradient(0,cy-radius,0,cy,new int[]{0x78FFFFFF,0x00FFFFFF},null,Shader.TileMode.CLAMP));
        area.set(cx-radius*.62f,cy-radius*.79f,cx+radius*.48f,cy-radius*.22f);
        canvas.drawOval(area,paint); paint.setShader(null);
        if (current) {
            float x=cx+radius*.68f,y=cy+radius*.66f;
            paint.setColor(0xE46FE3ED); canvas.drawCircle(x,y,d*.093f,paint);
            paint.setColor(Color.WHITE); paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(d*.025f);
            canvas.drawLine(x-d*.042f,y,x-d*.009f,y+d*.031f,paint);
            canvas.drawLine(x-d*.009f,y+d*.031f,x+d*.047f,y-d*.036f,paint);
        }
        canvas.restore();
    }
}
