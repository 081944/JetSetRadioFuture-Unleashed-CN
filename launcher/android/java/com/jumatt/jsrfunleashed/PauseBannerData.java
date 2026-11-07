package com.jumatt.jsrfunleashed;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;

/** The pause banner's original grey strip in SprNorm texture 1, in memory. */
final class PauseBannerData {
    static final int TITLE_WIDTH=256;
    static final class Assets {
        final int[] banner,title;
        Assets(int[] banner,int[] title) { this.banner=banner; this.title=title; }
    }
    static int[] load(File game) throws IOException {
        return loadBoth(game).banner;
    }
    static Assets loadBoth(File game) throws IOException {
        File disp=MapAsset.find(new File(game,"Media"),"Disp");
        File file=MapAsset.find(disp,"SprNorm.dat");
        if (file.length()<64 || file.length()>16*1024*1024) throw new IOException("Invalid UI resource size");
        ByteBuffer b=ByteBuffer.wrap(Files.readAllBytes(file.toPath())).order(ByteOrder.LITTLE_ENDIAN);
        if (u(b,0)!=0x544c554d || u(b,4)<1) throw new IOException("Missing UI container");
        int group=u(b,16);
        if (u(b,group)!=0x4d524f4e || u(b,group+4)<2) throw new IOException("Missing UI textures");
        int entry=group+16;
        int first=group+u(b,entry),firstSize=u(b,entry+8);
        entry=group+u(b,entry+4);
        int second=group+u(b,entry),secondSize=u(b,entry+8);
        int[] tile=region(b,first,firstSize,256,9,191,80,30);
        // The source patch contains a left fade. Repeating that whole patch
        // repeated the fade and atlas edge every few centimetres. Repeat only
        // its interior grid pixels, with ONE mirrored fade on the plate:
        // bright at the left-aligned name and transparent on the right.
        // This stays in memory and uses the user's original texture colors.
        int[] title=new int[TITLE_WIDTH*30];
        for (int y=0;y<30;y++) for (int x=0;x<TITLE_WIDTH;x++) {
            int color=tile[y*80+48+(x&15)];
            int alpha=Math.min(255,(TITLE_WIDTH-1-x)*255/160);
            title[y*TITLE_WIDTH+x]=(alpha<<24)|(color&0xFFFFFF);
        }
        int[] banner=region(b,second,secondSize,512,0,112,512,40);
        return new Assets(banner,title);
    }
    private static int[] region(ByteBuffer b,int texture,int size,int side,int x,int y,int width,int height) throws IOException {
        if (u(b,texture+20)!=side || u(b,texture+24)!=0x106 || size<32+side*side)
            throw new IOException("Unsupported pause texture");
        u(b,texture+size-4);
        int[] pixels=new int[width*height],colors=new int[4],alpha=new int[8];
        for (int by=y/4;by<(y+height+3)/4;++by) for (int bx=x/4;bx<(x+width+3)/4;++bx) {
            int p=texture+32+(by*(side/4)+bx)*16;
            alpha[0]=b.get(p)&255; alpha[1]=b.get(p+1)&255;
            for (int i=2;i<8;++i) alpha[i]=alpha[0]>alpha[1]
                    ? ((8-i)*alpha[0]+(i-1)*alpha[1])/7
                    : i<6 ? ((6-i)*alpha[0]+(i-1)*alpha[1])/5 : i==6 ? 0 : 255;
            long abits=0; for (int i=0;i<6;++i) abits|=(long)(b.get(p+2+i)&255)<<(i*8);
            colors[0]=rgb(b.getShort(p+8)&65535); colors[1]=rgb(b.getShort(p+10)&65535);
            colors[2]=mix(colors[0],colors[1]); colors[3]=mix(colors[1],colors[0]);
            int bits=u(b,p+12);
            for (int i=0;i<16;++i) {
                int dx=bx*4+i%4-x,dy=by*4+i/4-y;
                if (dx<0 || dy<0 || dx>=width || dy>=height) continue;
                int color=colors[(bits>>>(i*2))&3],a=alpha[(int)((abits>>>(i*3))&7)];
                pixels[dy*width+dx]=(a<<24)|color;
            }
        }
        return pixels;
    }
    private static int u(ByteBuffer b,int p) throws IOException {
        if (p<0 || p>b.limit()-4) throw new IOException("Truncated pause resource");
        return b.getInt(p);
    }
    private static int rgb(int c) {
        int r=c>>>11,g=(c>>>5)&63,blue=c&31;
        return (((r<<3)|(r>>>2))<<16)|(((g<<2)|(g>>>4))<<8)|((blue<<3)|(blue>>>2));
    }
    private static int mix(int a,int b) {
        return (((((a>>>16)&255)*2+((b>>>16)&255))/3)<<16)
                |(((((a>>>8)&255)*2+((b>>>8)&255))/3)<<8)|(((a&255)*2+(b&255))/3);
    }
}
