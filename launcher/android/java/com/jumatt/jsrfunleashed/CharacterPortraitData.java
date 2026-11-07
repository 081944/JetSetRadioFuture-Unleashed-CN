package com.jumatt.jsrfunleashed;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;

/** Decode the original small DXT1 charicon assets; no game images in the APK. */
final class CharacterPortraitData {
    // Original sub_34200: dword_21C4B0[dword_21C510[player ID]].
    private static final int[] ICONS={0,2,1,3,6,9,8,39,5,4,7,17,12,15,14,16,13,18,40,35,10,11,41,34};
    final int width;
    final int[] pixels;
    private CharacterPortraitData(int width,int[] pixels) { this.width=width; this.pixels=pixels; }
    static int iconForId(int id) {
        if (id<0 || id>=ICONS.length) throw new IllegalArgumentException("Invalid character ID");
        return ICONS[id];
    }
    static CharacterPortraitData load(File game,int id) throws IOException {
        if (id<0 || id>=ICONS.length) throw new IOException("Invalid character ID");
        File disp=MapAsset.find(new File(game,"Media"),"Disp");
        File folder=MapAsset.find(disp,"charicon");
        File file=MapAsset.find(folder,String.format(java.util.Locale.ROOT,"char%02d.dat",iconForId(id)));
        if (file.length()<128 || file.length()>65536) throw new IOException("Invalid portrait size");
        return decode(Files.readAllBytes(file.toPath()));
    }
    static CharacterPortraitData decode(byte[] bytes) throws IOException {
        ByteBuffer b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        int group=first(b,0,0x544c554d), texture=first(b,group,0x4d524f4e);
        check(b,texture,32);
        int width=b.getInt(texture+20), format=b.getInt(texture+24);
        if (width<4 || width>256 || (width&(width-1))!=0 || format!=0x105)
            throw new IOException("Unsupported portrait texture");
        int count=width*width;
        if (b.getInt(group+24)<32+count/2) throw new IOException("Truncated portrait texture");
        check(b,texture+32,count/2);
        int[] pixels=new int[count], colors=new int[4];
        int offset=texture+32;
        for (int y=0;y<width;y+=4) for (int x=0;x<width;x+=4,offset+=8) {
            int a=b.getShort(offset)&65535, c=b.getShort(offset+2)&65535;
            colors[0]=rgb565(a); colors[1]=rgb565(c);
            colors[2]=mix(colors[0],colors[1],a>c ? 2 : 1,a>c ? 3 : 2);
            colors[3]=a>c ? mix(colors[1],colors[0],2,3) : 0;
            int bits=b.getInt(offset+4);
            for (int p=0;p<16;++p) pixels[(y+p/4)*width+x+p%4]=colors[(bits>>>(p*2))&3];
        }
        return new CharacterPortraitData(width,pixels);
    }
    private static int first(ByteBuffer b,int base,int magic) throws IOException {
        check(b,base,32);
        int count=b.getInt(base+4),relative=b.getInt(base+16),length=b.getInt(base+24);
        if (b.getInt(base)!=magic || count<1 || count>256 || relative<32 || relative>b.limit()-base)
            throw new IOException("Invalid portrait container");
        int start=base+relative;
        check(b,start,length);
        return start;
    }
    private static void check(ByteBuffer b,int at,int size) throws IOException {
        if (at<0 || size<0 || at>b.limit()-size) throw new IOException("Truncated portrait");
    }
    private static int rgb565(int c) {
        int r=(c>>>11)&31,g=(c>>>5)&63,blue=c&31;
        return 0xff000000|(((r<<3)|(r>>>2))<<16)|(((g<<2)|(g>>>4))<<8)|((blue<<3)|(blue>>>2));
    }
    private static int mix(int a,int b,int weight,int divisor) {
        int other=divisor-weight;
        int r=(((a>>>16)&255)*weight+((b>>>16)&255)*other)/divisor;
        int g=(((a>>>8)&255)*weight+((b>>>8)&255)*other)/divisor;
        int blue=((a&255)*weight+(b&255)*other)/divisor;
        return 0xff000000|(r<<16)|(g<<8)|blue;
    }
}
