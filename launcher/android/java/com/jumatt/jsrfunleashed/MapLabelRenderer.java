package com.jumatt.jsrfunleashed;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.opengl.GLES20;
import android.opengl.GLUtils;
import android.opengl.Matrix;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.HashMap;

/** Screen-facing text: readable at every map angle, without changing positions
 * supplied by the game. GL resources belong to the companion context only. */
final class MapLabelRenderer {
    private final HashMap<Integer,int[]> labels=new HashMap<>();
    private final FloatBuffer quad=ByteBuffer.allocateDirect(64).order(ByteOrder.nativeOrder()).asFloatBuffer();
    private final float[] point=new float[4], clip=new float[4];
    private int program, textUniform;
    private static int shader(int type,String source) {
        int id=GLES20.glCreateShader(type);
        GLES20.glShaderSource(id,source); GLES20.glCompileShader(id);
        int[] ok=new int[1]; GLES20.glGetShaderiv(id,GLES20.GL_COMPILE_STATUS,ok,0);
        if (ok[0]==0) throw new IllegalStateException(GLES20.glGetShaderInfoLog(id));
        return id;
    }
    void create() {
        labels.clear();
        int vs=shader(GLES20.GL_VERTEX_SHADER,"attribute vec2 aPosition; attribute vec2 aUv; varying vec2 vUv;"
                +"void main(){gl_Position=vec4(aPosition,0.0,1.0);vUv=aUv;}");
        int fs=shader(GLES20.GL_FRAGMENT_SHADER,"precision mediump float; uniform sampler2D uText; varying vec2 vUv;"
                +"void main(){gl_FragColor=texture2D(uText,vUv);}");
        program=GLES20.glCreateProgram(); GLES20.glAttachShader(program,vs); GLES20.glAttachShader(program,fs);
        GLES20.glBindAttribLocation(program,0,"aPosition"); GLES20.glBindAttribLocation(program,1,"aUv");
        GLES20.glLinkProgram(program);
        int[] ok=new int[1]; GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,ok,0);
        if (ok[0]==0) throw new IllegalStateException(GLES20.glGetProgramInfoLog(program));
        textUniform=GLES20.glGetUniformLocation(program,"uText");
        GLES20.glDeleteShader(vs); GLES20.glDeleteShader(fs);
    }
    private int[] label(int kind,String text) {
        int[] cached=labels.get(kind); if (cached!=null) return cached;
        Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setTextSize(32); paint.setTypeface(Typeface.create("sans-serif",Typeface.BOLD));
        // Long destination names get two lines instead of shrinking to illegibility.
        String[] lines=text.length()>9 ? new String[] {text.substring(0,(text.length()+1)/2),text.substring((text.length()+1)/2)}
                : new String[] {text};
        float widest=0; for (String line:lines) widest=Math.max(widest,paint.measureText(line));
        Paint.FontMetrics metrics=paint.getFontMetrics();
        int lineHeight=(int)Math.ceil(metrics.descent-metrics.ascent)+4;
        int w=(int)Math.ceil(widest)+24, h=lineHeight*lines.length+16;
        Bitmap bitmap=Bitmap.createBitmap(w,h,Bitmap.Config.ARGB_8888);
        Canvas canvas=new Canvas(bitmap);
        paint.setColor(Color.argb(190,8,48,51)); canvas.drawRoundRect(0,0,w,h,10,10,paint);
        paint.setColor(Color.WHITE);
        for (int i=0;i<lines.length;++i) canvas.drawText(lines[i],(w-paint.measureText(lines[i]))*.5f,8-metrics.ascent+i*lineHeight,paint);
        int[] id=new int[1]; GLES20.glGenTextures(1,id,0); GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,id[0]);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_MIN_FILTER,GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_MAG_FILTER,GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_WRAP_S,GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_WRAP_T,GLES20.GL_CLAMP_TO_EDGE);
        GLUtils.texImage2D(GLES20.GL_TEXTURE_2D,0,bitmap,0); bitmap.recycle();
        cached=new int[] {id[0],w,h}; labels.put(kind,cached); return cached;
    }
    boolean draw(int kind,float x,float y,float z,float[] vp,int width,int height) {
        String text=MapPlaceNames.forKind(kind); if (text==null) return false;
        point[0]=x; point[1]=y; point[2]=z; point[3]=1;
        Matrix.multiplyMV(clip,0,vp,0,point,0);
        if (clip[3]<=0 || Math.abs(clip[0])>clip[3] || Math.abs(clip[1])>clip[3]
                || Math.abs(clip[2])>clip[3]) return true;
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        int[] texture=label(kind,text);
        float scale=Math.min(height/960f,width*.36f/texture[1]);
        float halfW=texture[1]*scale/width, halfH=texture[2]*scale/height;
        float px=Math.max(-1+halfW,Math.min(1-halfW,clip[0]/clip[3]));
        float py=Math.max(-1+halfH,Math.min(1-halfH,clip[1]/clip[3]));
        quad.clear(); quad.put(new float[] {px-halfW,py-halfH,0,1, px+halfW,py-halfH,1,1,
                px-halfW,py+halfH,0,0, px+halfW,py+halfH,1,0}).position(0);
        GLES20.glUseProgram(program); GLES20.glUniform1i(textUniform,0);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,texture[0]);
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER,0);
        GLES20.glDisable(GLES20.GL_DEPTH_TEST); GLES20.glDisable(GLES20.GL_CULL_FACE);
        // Canvas bitmaps contain premultiplied alpha, unlike the model colors.
        GLES20.glBlendFunc(GLES20.GL_ONE,GLES20.GL_ONE_MINUS_SRC_ALPHA);
        GLES20.glEnableVertexAttribArray(0); GLES20.glEnableVertexAttribArray(1); GLES20.glDisableVertexAttribArray(2);
        quad.position(0); GLES20.glVertexAttribPointer(0,2,GLES20.GL_FLOAT,false,16,quad);
        quad.position(2); GLES20.glVertexAttribPointer(1,2,GLES20.GL_FLOAT,false,16,quad);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP,0,4);
        GLES20.glBlendFunc(GLES20.GL_SRC_ALPHA,GLES20.GL_ONE_MINUS_SRC_ALPHA);
        return true;
    }
}
