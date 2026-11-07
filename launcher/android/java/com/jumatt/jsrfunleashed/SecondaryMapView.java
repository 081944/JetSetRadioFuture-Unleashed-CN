package com.jumatt.jsrfunleashed;

import android.content.Context;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.opengl.Matrix;
import android.util.Log;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.IdentityHashMap;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** Independent 30 Hz companion renderer. It never submits game input or touches
 * the primary Vulkan surface. Original map/symbol meshes stay on the GPU. */
final class SecondaryMapView extends GLSurfaceView implements GLSurfaceView.Renderer {
    private static final String TAG = "JSRF-Map";
    static native void nativeEnable(boolean enabled);
    private static native int nativeSnapshot(ByteBuffer buffer);
    private static native String nativeObjective();
    interface Status {
        void show(String text);
        default void caption(String title,String objective) { }
    }
    private final Status status;
    private final File disp;
    private final ByteBuffer buffer = ByteBuffer.allocateDirect((16+1024*6)*4).order(ByteOrder.nativeOrder());
    private final FloatBuffer live = buffer.asFloatBuffer();
    private final IdentityHashMap<MapAsset.Model,Integer> buffers = new IdentityHashMap<>();
    private final MapAsset.Model[] symbols = new MapAsset.Model[35];
    private MapAsset.Model floor;
    private MapAsset district;
    private int districtId = -1, stageId = -1;
    private int program, matrixUniform, normalUniform, lightUniform, backgroundProgram, gridUniform;
    private int width = 1, height = 1;
    private float fitDistance;
    private final float[] view = new float[16], projection = new float[16], transform = new float[16];
    private final float[] vp = new float[16], mvp = new float[16];
    private final float[] modelView = new float[16], normals = new float[9];
    private float zoom = 1;
    private float orbitPitch, orbitYaw;
    private static final float ORBIT_LIMIT=35f;
    private final float[] orbit={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    private final MapAsset.Model playerArrow=playerArrow();
    private final MapLabelRenderer labels=new MapLabelRenderer();
    private final FloatBuffer quad=ByteBuffer.allocateDirect(32).order(ByteOrder.nativeOrder()).asFloatBuffer();
    private float lastX, lastY;
    private boolean gesturesActive;
    private boolean symbolsLoaded;
    private String lastStatus;
    private String lastTitle="",lastObjective="";
    private final ScaleGestureDetector pinch;
    private final GestureDetector tap;

    SecondaryMapView(Context context, File game, Status callback) {
        super(context);
        status = callback;
        disp = new File(game,"Media/Disp");
        quad.put(new float[] {-1,-1,1,-1,-1,1,1,1}).position(0);
        setEGLContextClientVersion(2);
        setEGLConfigChooser(8,8,8,0,16,0);
        setPreserveEGLContextOnPause(true);
        setRenderer(this);
        setRenderMode(RENDERMODE_WHEN_DIRTY);
        pinch = new ScaleGestureDetector(context,new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override public boolean onScale(ScaleGestureDetector detector) {
                final float factor = detector.getScaleFactor();
                queueEvent(() -> zoom = Math.max(.35f,Math.min(4f,zoom*factor)));
                requestRender(); return true;
            }
        });
        tap = new GestureDetector(context,new GestureDetector.SimpleOnGestureListener() {
            @Override public boolean onDown(MotionEvent e) { return true; }
            @Override public boolean onDoubleTap(MotionEvent e) {
                queueEvent(() -> { resetOrbit(); zoom = 1; });
                requestRender(); return true;
            }
        });
    }
    @Override public boolean onTouchEvent(MotionEvent e) {
        pinch.onTouchEvent(e); tap.onTouchEvent(e);
        switch (e.getActionMasked()) {
        case MotionEvent.ACTION_DOWN:
            gesturesActive = true; lastX=e.getX(); lastY=e.getY(); break;
        case MotionEvent.ACTION_POINTER_DOWN:
            gesturesActive = false; break;
        case MotionEvent.ACTION_POINTER_UP:
            // A fresh DOWN starts rotation after a pinch; no jump to a different finger.
            gesturesActive = false; break;
        case MotionEvent.ACTION_MOVE:
            if (gesturesActive && e.getPointerCount()==1 && !pinch.isInProgress()) {
                float dx=(e.getX()-lastX)*240f/Math.max(1,getWidth());
                float dy=(e.getY()-lastY)*150f/Math.max(1,getHeight());
                queueEvent(() -> {
                    orbitPitch=Math.max(-ORBIT_LIMIT,Math.min(ORBIT_LIMIT,orbitPitch-dy));
                    orbitYaw=Math.max(-ORBIT_LIMIT,Math.min(ORBIT_LIMIT,orbitYaw-dx));
                    float angle=(float)Math.hypot(orbitPitch,orbitYaw);
                    if (angle>ORBIT_LIMIT) {
                        orbitPitch*=ORBIT_LIMIT/angle; orbitYaw*=ORBIT_LIMIT/angle;
                        angle=ORBIT_LIMIT;
                    }
                    // One rotation whose axis lies in the screen plane:
                    // zero twist around its normal, even for diagonal drags.
                    Matrix.setIdentityM(orbit,0);
                    if (angle>0.0001f) Matrix.rotateM(orbit,0,angle,orbitPitch,orbitYaw,0);
                });
                requestRender();
            }
            lastX=e.getX(); lastY=e.getY(); break;
        case MotionEvent.ACTION_UP: case MotionEvent.ACTION_CANCEL:
            gesturesActive = false; break;
        default: break;
        }
        return true;
    }
    private void say(String text) {
        if (!text.equals(lastStatus)) { lastStatus=text; post(() -> status.show(text)); }
    }
    private void caption(String title,String objective) {
        if (!title.equals(lastTitle) || !objective.equals(lastObjective)) {
            lastTitle=title; lastObjective=objective;
            post(() -> status.caption(title,objective));
        }
    }
    private void resetOrbit() {
        orbitPitch=orbitYaw=0;
        Matrix.setIdentityM(orbit,0);
    }
    private static int shader(int type,String text) {
        int id=GLES20.glCreateShader(type); GLES20.glShaderSource(id,text); GLES20.glCompileShader(id);
        int[] ok=new int[1]; GLES20.glGetShaderiv(id,GLES20.GL_COMPILE_STATUS,ok,0);
        if (ok[0]==0) throw new IllegalStateException(GLES20.glGetShaderInfoLog(id));
        return id;
    }
    private static MapAsset.Model playerArrow() {
        float[][] points={{0,0,1.5f},{-.65f,0,-.8f},{0,0,-.35f},{.65f,0,-.8f}};
        int[] indices={0,1,2,0,2,3}; float[] out=new float[indices.length*10]; int at=0;
        for (int index:indices) {
            for (float value:points[index]) out[at++]=value;
            out[at++]=0; out[at++]=1; out[at++]=0;
            out[at++]=1; out[at++]=.94f; out[at++]=.02f; out[at++]=1;
        }
        return new MapAsset.Model(out);
    }
    @Override public void onSurfaceCreated(GL10 unused,EGLConfig config) {
        buffers.clear();
        labels.create();
        int vs=shader(GLES20.GL_VERTEX_SHADER,
            "attribute vec3 aPosition; attribute vec3 aNormal; attribute vec4 aColor;"
            +"uniform mat4 uMvp; uniform mat3 uNormal; uniform float uLight; varying vec4 vColor;"
            +"void main(){gl_Position=uMvp*vec4(aPosition,1.0);"
            +"vec3 normal=normalize(uNormal*aNormal);"
            +"float diffuse=max(normal.z,0.0);"
            +"vColor=vec4(aColor.rgb*mix(1.0,diffuse,uLight),aColor.a);}");
        int fs=shader(GLES20.GL_FRAGMENT_SHADER,
            "precision mediump float; varying vec4 vColor; void main(){if(vColor.a<=0.0)discard;gl_FragColor=vColor;}");
        program=GLES20.glCreateProgram(); GLES20.glAttachShader(program,vs); GLES20.glAttachShader(program,fs);
        GLES20.glBindAttribLocation(program,0,"aPosition"); GLES20.glBindAttribLocation(program,1,"aNormal");
        GLES20.glBindAttribLocation(program,2,"aColor"); GLES20.glLinkProgram(program);
        int[] ok=new int[1]; GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,ok,0);
        if (ok[0]==0) throw new IllegalStateException(GLES20.glGetProgramInfoLog(program));
        GLES20.glDeleteShader(vs); GLES20.glDeleteShader(fs);
        matrixUniform=GLES20.glGetUniformLocation(program,"uMvp"); lightUniform=GLES20.glGetUniformLocation(program,"uLight");
        normalUniform=GLES20.glGetUniformLocation(program,"uNormal");
        vs=shader(GLES20.GL_VERTEX_SHADER,"attribute vec2 aPosition; void main(){gl_Position=vec4(aPosition,0.0,1.0);}");
        fs=shader(GLES20.GL_FRAGMENT_SHADER,
            "precision mediump float; uniform float uUnit; void main(){"
            +"vec2 cell=mod(gl_FragCoord.xy,uUnit); float line=1.0-step(1.1,min(cell.x,cell.y));"
            +"vec2 big=mod(gl_FragCoord.xy,uUnit*4.0); float major=1.0-step(1.6,min(big.x,big.y));"
            +"vec3 color=mix(vec3(0.27,0.56,0.56),vec3(0.19,0.43,0.44),line);"
            +"gl_FragColor=vec4(mix(color,vec3(0.14,0.35,0.36),major),1.0);}");
        backgroundProgram=GLES20.glCreateProgram();
        GLES20.glAttachShader(backgroundProgram,vs); GLES20.glAttachShader(backgroundProgram,fs);
        GLES20.glBindAttribLocation(backgroundProgram,0,"aPosition"); GLES20.glLinkProgram(backgroundProgram);
        GLES20.glGetProgramiv(backgroundProgram,GLES20.GL_LINK_STATUS,ok,0);
        if (ok[0]==0) throw new IllegalStateException(GLES20.glGetProgramInfoLog(backgroundProgram));
        GLES20.glDeleteShader(vs); GLES20.glDeleteShader(fs);
        gridUniform=GLES20.glGetUniformLocation(backgroundProgram,"uUnit");
        GLES20.glClearColor(.035f,.22f,.23f,1);
        GLES20.glDisable(GLES20.GL_CULL_FACE);
        GLES20.glDepthFunc(GLES20.GL_LEQUAL);
    }
    @Override public void onSurfaceChanged(GL10 unused,int w,int h) {
        width=w; height=h; fitDistance=0; GLES20.glViewport(0,0,w,h);
    }
    private void clearBuffers() {
        int[] id=new int[1];
        for (int value : buffers.values()) { id[0]=value; GLES20.glDeleteBuffers(1,id,0); }
        buffers.clear();
    }
    private void load(int stage) throws Exception {
        int group=stage>>>16, area=stage&65535;
        if (group!=districtId) {
            district=new MapAsset(MapAsset.find(MapAsset.find(disp,"map"),String.format(java.util.Locale.ROOT,"Stage%02d.dat",group)));
            districtId=group;
        }
        floor=district.model(area+2);
        if (floor==null) throw new java.io.IOException("Missing stage map " + stage);
        if (!symbolsLoaded) {
            MapAsset sprites=new MapAsset(MapAsset.find(disp,"SprNorm.dat"));
            for (int k=0;k<symbols.length;++k) symbols[k]=sprites.model(k<12 ? k+108 : k+178);
            symbolsLoaded=true;
        }
        clearBuffers(); resetOrbit(); zoom=1; fitDistance=0;
        Log.i(TAG,"Loaded original map " + group+":"+area+", "+floor.count/3+" triangles");
    }
    private void draw(MapAsset.Model model,float lighting) {
        if (model==null) return;
        Integer id=buffers.get(model);
        if (id==null) {
            int[] out=new int[1]; GLES20.glGenBuffers(1,out,0); id=out[0];
            GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER,id);
            model.vertices.position(0);
            GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER,model.count*40,model.vertices,GLES20.GL_STATIC_DRAW);
            buffers.put(model,id);
        } else GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER,id);
        Matrix.multiplyMM(mvp,0,vp,0,transform,0);
        Matrix.multiplyMM(modelView,0,view,0,transform,0);
        for (int col=0;col<3;++col) for (int row=0;row<3;++row) normals[col*3+row]=modelView[col*4+row];
        GLES20.glUniformMatrix3fv(normalUniform,1,false,normals,0);
        GLES20.glUniformMatrix4fv(matrixUniform,1,false,mvp,0); GLES20.glUniform1f(lightUniform,lighting);
        GLES20.glVertexAttribPointer(0,3,GLES20.GL_FLOAT,false,40,0);
        GLES20.glVertexAttribPointer(1,3,GLES20.GL_FLOAT,false,40,12);
        GLES20.glVertexAttribPointer(2,4,GLES20.GL_FLOAT,false,40,24);
        if (model.opaqueCount>0) GLES20.glDrawArrays(GLES20.GL_TRIANGLES,0,model.opaqueCount);
        if (model.count>model.opaqueCount) {
            GLES20.glDepthMask(false);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES,model.opaqueCount,model.count-model.opaqueCount);
            GLES20.glDepthMask(true);
        }
    }
    @Override public void onDrawFrame(GL10 unused) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT|GLES20.GL_DEPTH_BUFFER_BIT);
        if (nativeSnapshot(buffer)==0 || live.get(0)==0) {
            say("进入游戏后显示地图"); caption("",""); return;
        }
        int stage=(int)live.get(1);
        caption(MapPlaceNames.forStage(stage),MapObjective.display(nativeObjective()));
        if (stage!=stageId) {
            stageId=stage; floor=null;
            try { load(stage); }
            catch (Exception ex) { Log.e(TAG,"Cannot load original map",ex); say("地图加载失败："+ex.getMessage()); }
        }
        if (floor==null) return;
        say("");
        GLES20.glDisable(GLES20.GL_DEPTH_TEST); GLES20.glDisable(GLES20.GL_CULL_FACE);
        GLES20.glUseProgram(backgroundProgram); GLES20.glUniform1f(gridUniform,Math.max(20,height/30f));
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER,0);
        GLES20.glEnableVertexAttribArray(0); GLES20.glDisableVertexAttribArray(1); GLES20.glDisableVertexAttribArray(2);
        quad.position(0); GLES20.glVertexAttribPointer(0,2,GLES20.GL_FLOAT,false,0,quad);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP,0,4);
        float cx=(floor.min[0]+floor.max[0])*.5f, cy=(floor.min[1]+floor.max[1])*.5f, cz=(floor.min[2]+floor.max[2])*.5f;
        float rx=floor.max[0]-floor.min[0], ry=floor.max[1]-floor.min[1], rz=floor.max[2]-floor.min[2];
        float radius=Math.max(1,(float)Math.sqrt(rx*rx+ry*ry+rz*rz)*.5f);
        float aspect=(float)width/Math.max(1,height);
        if (fitDistance==0) {
            Matrix.setIdentityM(view,0); Matrix.scaleM(view,0,1,1,-1);
            Matrix.rotateM(view,0,live.get(10),0,0,1);
            Matrix.rotateM(view,0,live.get(9),0,1,0);
            Matrix.rotateM(view,0,live.get(8),1,0,0);
            // Fit actual map vertices at the original direction, excluding the
            // removed pause menu. Keep this distance stable during dragging.
            float fit=1;
            for (int v=0;v<floor.count;++v) {
                float x=floor.vertices.get(v*10)-cx, y=floor.vertices.get(v*10+1)-cy, z=floor.vertices.get(v*10+2)-cz;
                float vx=view[0]*x+view[4]*y+view[8]*z;
                float vy=view[1]*x+view[5]*y+view[9]*z;
                float vz=view[2]*x+view[6]*y+view[10]*z;
                fit=Math.max(fit,vz+Math.max(Math.abs(vx)/aspect,Math.abs(vy))*1.14f/.767327f);
            }
            fitDistance=fit;
        }
        float distance=fitDistance/zoom;
        // The original uses a left-handed camera looking along +Z, followed
        // by Z/Y/X model rotation. An ordinary GLES lookAt from +Z mirrors
        // the map vertically. Reflect camera Z explicitly, including winding.
        Matrix.setIdentityM(view,0); Matrix.translateM(view,0,0,0,-distance);
        Matrix.scaleM(view,0,1,1,-1);
        Matrix.multiplyMM(modelView,0,view,0,orbit,0);
        System.arraycopy(modelView,0,view,0,16);
        Matrix.rotateM(view,0,live.get(10),0,0,1);
        Matrix.rotateM(view,0,live.get(9),0,1,0);
        Matrix.rotateM(view,0,live.get(8),1,0,0);
        Matrix.translateM(view,0,-cx,-cy,-cz);
        Matrix.perspectiveM(projection,0,75,aspect,Math.max(.01f,distance*.001f),distance+radius*6);
        // Shift every world element together (including labels and markers)
        // 8% left, leaving room for the two portrait columns on the right.
        projection[8]=.16f;
        Matrix.multiplyMM(vp,0,projection,0,view,0);
        GLES20.glUseProgram(program);
        for (int i=0;i<3;++i) GLES20.glEnableVertexAttribArray(i);
        GLES20.glEnable(GLES20.GL_DEPTH_TEST); GLES20.glEnable(GLES20.GL_CULL_FACE);
        GLES20.glEnable(GLES20.GL_BLEND); GLES20.glBlendFunc(GLES20.GL_SRC_ALPHA,GLES20.GL_ONE_MINUS_SRC_ALPHA);
        // Camera-Z reflection reverses winding. Alpha-zero outer-shell parts
        // have already been excluded by the asset loader.
        GLES20.glFrontFace(GLES20.GL_CW); GLES20.glCullFace(GLES20.GL_BACK);
        Matrix.setIdentityM(transform,0); draw(floor,1);
        GLES20.glDisable(GLES20.GL_DEPTH_TEST); GLES20.glDisable(GLES20.GL_CULL_FACE);
        float perPixel=2*distance*.767327f/Math.max(1,height);
        int count=Math.min(1024,Math.max(0,(int)live.get(3)));
        // Pickups, names, then the player: the arrow must remain easy to find.
        for (int pass=0;pass<3;++pass) for (int i=0;i<count;++i) {
            int p=16+i*6, kind=(int)live.get(p);
            if (kind<0 || kind>=symbols.length || (kind==3 ? pass!=2 : kind>=12 ? pass!=1 : pass!=0)) continue;
            float x=live.get(p+1)+floor.markerOffset[0], y=live.get(p+2)+floor.markerOffset[1], z=live.get(p+3)+floor.markerOffset[2];
            if (kind>=12 && labels.draw(kind,x,y,z,vp,width,height)) continue;
            MapAsset.Model icon=kind==3 ? playerArrow : symbols[kind]; if (icon==null) continue;
            GLES20.glUseProgram(program);
            for (int attribute=0;attribute<3;++attribute) GLES20.glEnableVertexAttribArray(attribute);
            if (kind<12 && kind!=3) {
                GLES20.glEnable(GLES20.GL_DEPTH_TEST); GLES20.glEnable(GLES20.GL_CULL_FACE);
                // Billboard inverse rotation cancels the camera reflection.
                GLES20.glFrontFace(GLES20.GL_CCW);
            } else {
                GLES20.glDisable(GLES20.GL_DEPTH_TEST); GLES20.glDisable(GLES20.GL_CULL_FACE);
            }
            Matrix.setIdentityM(transform,0); Matrix.translateM(transform,0,x,y,z);
            float size=kind>=12 ? 27 : kind==3 ? 48 : 19;
            float extent=Math.max(.1f,icon.max[1]-icon.min[1]);
            if (kind==3) {
                Matrix.rotateM(transform,0,live.get(p+4),0,1,0);
                extent=Math.max(.1f,Math.max(icon.max[0]-icon.min[0],icon.max[2]-icon.min[2]));
            } else {
                // Inverse view rotation, to keep names/icons facing the lower screen.
                transform[0]=view[0]; transform[1]=view[4]; transform[2]=view[8];
                transform[4]=view[1]; transform[5]=view[5]; transform[6]=view[9];
                transform[8]=view[2]; transform[9]=view[6]; transform[10]=view[10];
            }
            float scale=perPixel*size/extent;
            Matrix.scaleM(transform,0,scale,scale,scale); draw(icon,kind==3 ? 0 : 1);
        }
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER,0);
    }
}
