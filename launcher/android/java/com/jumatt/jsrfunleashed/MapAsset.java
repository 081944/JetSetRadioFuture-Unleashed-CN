package com.jumatt.jsrfunleashed;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/** The game's own Disp/map models and SprNorm map symbols, without the pause
 * menu. Parsing is independent of Android so it can be checked against every
 * stage on the host. All file offsets/counts are bounded before allocation. */
final class MapAsset {
    static final class Model {
        final FloatBuffer vertices;
        final int count;
        final int opaqueCount;
        final float[] min = {Float.POSITIVE_INFINITY, Float.POSITIVE_INFINITY, Float.POSITIVE_INFINITY};
        final float[] max = {Float.NEGATIVE_INFINITY, Float.NEGATIVE_INFINITY, Float.NEGATIVE_INFINITY};
        final float[] markerOffset = new float[3];
        Model(float[] data) {
            this(data,data.length/10);
        }
        Model(float[] data,int opaqueCount) {
            count = data.length / 10;
            this.opaqueCount=opaqueCount;
            vertices = ByteBuffer.allocateDirect(data.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
            vertices.put(data).position(0);
            for (int p = 0; p < data.length; p += 10) for (int k = 0; k < 3; ++k) {
                min[k] = Math.min(min[k], data[p+k]); max[k] = Math.max(max[k], data[p+k]);
            }
        }
    }
    private final ByteBuffer data;
    private final int[] models;
    private final int[] ends;

    MapAsset(File file) throws IOException {
        long size = file.length();
        if (size < 32 || size > 16 * 1024 * 1024) throw new IOException("Invalid map asset size: " + file);
        data = ByteBuffer.wrap(Files.readAllBytes(file.toPath())).order(ByteOrder.LITTLE_ENDIAN);
        int[][] groups = chunks(0);
        if (groups.length < 2) throw new IOException("Missing model group");
        int[][] entries = chunks(groups[1][0]);
        models = new int[entries.length]; ends = new int[entries.length];
        for (int i = 0; i < entries.length; ++i) if (entries[i][2] == 0 && entries[i][1] > 0) {
            models[i] = entries[i][0]; ends[i] = checkedEnd(entries[i][0], entries[i][1]);
        }
    }
    static File find(File parent, String name) throws IOException {
        File exact = new File(parent, name);
        if (exact.exists()) return exact;
        File[] files = parent.listFiles();
        if (files != null) for (File f : files) if (f.getName().equalsIgnoreCase(name)) return f;
        throw new IOException("Missing " + name);
    }
    private int checkedEnd(int p, int length) throws IOException {
        if (p < 0 || length < 0 || p > data.limit() - length) throw new IOException("Map asset offset out of range");
        return p + length;
    }
    private int u(int p) throws IOException { checkedEnd(p,4); return data.getInt(p); }
    private float f(int p) throws IOException {
        checkedEnd(p,4); float value = data.getFloat(p);
        if (!Float.isFinite(value) || Math.abs(value) > 1000000) throw new IOException("Invalid map coordinate");
        return value;
    }
    private int[][] chunks(int base) throws IOException {
        int magic = u(base);
        if (magic != 0x544c554d && magic != 0x4d524f4e) throw new IOException("Invalid map chunk");
        int n = u(base+4);
        if (n < 0 || n > 4096) throw new IOException("Invalid map chunk count");
        int[][] out = new int[n][3];
        int p = base+16;
        for (int i = 0; i < n; ++i) {
            checkedEnd(p,16);
            out[i][0] = checkedEnd(base,u(p)); out[i][1] = u(p+8); out[i][2] = u(p+12);
            checkedEnd(out[i][0],out[i][1]);
            int next = checkedEnd(base,u(p+4));
            if (i+1 < n && next <= p) throw new IOException("Cyclic map chunk");
            p = next;
        }
        return out;
    }
    int modelCount() { return models.length; }
    Model model(int index) throws IOException {
        if (index < 0 || index >= models.length || models[index] == 0) return null;
        int start = models[index], end = ends[index], base = start+16;
        if (u(start) != 0x424c444d) throw new IOException("Invalid map model"); // MDLB
        int nodes = u(start+4);
        if (nodes < 1 || nodes > 1024 || (long)base + nodes * 128L > end) throw new IOException("Invalid map nodes");
        List<float[]> opaque = new ArrayList<>(), translucent = new ArrayList<>();
        int total = 0, opaqueFloats=0;
        for (int n = 0; n < nodes; ++n) {
            int node = base+n*128, off = u(node);
            if (off == 0) continue;
            int geom = checkedEnd(base,off);
            if (geom + 28L > end) throw new IOException("Invalid map geometry");
            int nv = u(geom+4), fvf = u(geom+8), stride = u(geom+12), ni = u(geom+20), primitive = u(geom+24);
            if (nv < 1 || nv > 65536 || ni < 0 || ni > 1000000 || stride != 24 || fvf != 18 || primitive < 0 || primitive > 2)
                throw new IOException("Unsupported map geometry: model="+index+" node="+n+" vertices="+nv+" fvf="+fvf+" stride="+stride+" indices="+ni+" primitive="+primitive);
            int vp = checkedEnd(base,u(geom)), ip = checkedEnd(base,u(geom+16));
            if ((long)vp+nv*24L > end || (long)ip+ni*2L > end) throw new IOException("Invalid map vertices");
            int materials = u(node+116), mp = checkedEnd(base,u(node+112));
            if (materials < 0 || materials > 1024 || (long)mp + materials * 32L > end) throw new IOException("Invalid map materials");
            for (int j = 0; j < materials; ++j) {
                int part = mp+j*32, triangles = u(part), first = u(part+4), matIndex = u(part+16);
                if (triangles < 0 || triangles > 1000000 || first < 0 || matIndex < 0 || matIndex > 4096)
                    throw new IOException("Invalid map primitive");
                int count = primitive == 0 ? triangles*3 : primitive == 1 ? triangles+2 : triangles*4;
                // Some original strip symbols omit the terminal index from the
                // header count. The material range is authoritative, but still
                // must fit wholly within this model and contain valid vertices.
                if ((long)first+count > ni+(primitive==1 ? 2L : 0L) || (long)ip+((long)first+count)*2 > end)
                    throw new IOException("Invalid map indices: model="+index+" node="+n);
                int palette = checkedEnd(base,u(node+4));
                int colorPos = checkedEnd(palette,matIndex*20);
                if (colorPos + 20L > end) throw new IOException("Invalid map palette");
                int color = u(colorPos);
                // sub_00186A30 converts this ARGB diffuse value, including A.
                // Maps contain invisible root faces and a translucent outer
                // shell. Making either opaque hides the colored interior.
                float alpha=(color>>>24)/255f;
                if (alpha==0) continue;
                float[] rgba = {((color>>>16)&255)/255f, ((color>>>8)&255)/255f, (color&255)/255f, alpha};
                int expanded = primitive == 2 ? triangles*6 : triangles*3;
                if ((long)total + expanded*10L > 8000000) throw new IOException("Map too large");
                float[] vertices = new float[expanded*10];
                int dst = 0;
                for (int t = 0; t < triangles; ++t) {
                    int width = primitive == 2 ? 6 : 3;
                    for (int v = 0; v < width; ++v) {
                        int at;
                        if (primitive == 0) at = first+t*3+v;
                        else if (primitive == 1) at = first+t+((t&1) != 0 && v < 2 ? 1-v : v);
                        else at = first+t*4+(v==3 ? 0 : v==4 ? 2 : v==5 ? 3 : v);
                        int vertex = data.getShort(ip+at*2)&65535;
                        if (vertex >= nv) throw new IOException("Map vertex index out of range");
                        for (int k = 0; k < 6; ++k) vertices[dst++] = f(vp+vertex*24+k*4);
                        for (float c : rgba) vertices[dst++] = c;
                    }
                }
                if (alpha==1) { opaque.add(vertices); opaqueFloats+=vertices.length; }
                else translucent.add(vertices);
                total += vertices.length;
            }
        }
        if (total == 0) return null;
        float[] combined = new float[total]; int at = 0;
        for (float[] piece : opaque) { System.arraycopy(piece,0,combined,at,piece.length); at += piece.length; }
        for (float[] piece : translucent) { System.arraycopy(piece,0,combined,at,piece.length); at += piece.length; }
        Model result = new Model(combined,opaqueFloats/10);
        /* The original map adds the first child node's translation to icons,
         * while the floor mesh's vertex positions are already transformed. */
        if (nodes > 1) for (int k = 0; k < 3; ++k) result.markerOffset[k] = f(base+128+32+k*4);
        return result;
    }
}
