package com.jumatt.jsrfunleashed;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;

public final class CharacterPortraitTest {
    private static int xbeWord(ByteBuffer x,int va) {
        int base=x.getInt(0x104),sections=x.getInt(0x11c),table=x.getInt(0x120)-base;
        for (int i=0;i<sections;++i) {
            int p=table+i*56,start=x.getInt(p+4),raw=x.getInt(p+12),size=x.getInt(p+16);
            if (va>=start && va-start<=size-4) return x.getInt(raw+va-start);
        }
        throw new AssertionError("Unmapped XBE address");
    }
    private static void rejected(byte[] bytes) throws Exception {
        try { CharacterPortraitData.decode(bytes); throw new AssertionError("Accepted malformed portrait"); }
        catch (IOException expected) { }
    }
    public static void main(String[] args) throws Exception {
        File game=new File(args[0]);
        ByteBuffer xbe=ByteBuffer.wrap(Files.readAllBytes(new File(args[1]).toPath())).order(ByteOrder.LITTLE_ENDIAN);
        for (int id=0;id<24;++id) {
            int expected=xbeWord(xbe,0x21C4B0+xbeWord(xbe,0x21C510+id*4)*4);
            if (CharacterPortraitData.iconForId(id)!=expected) throw new AssertionError("Original ID/portrait mapping "+id);
            CharacterPortraitData image=CharacterPortraitData.load(game,id);
            if (image.width<32 || image.width>256 || image.pixels.length!=image.width*image.width)
                throw new AssertionError("Portrait dimensions "+id);
            int min=0xffffff,max=0;
            for (int pixel:image.pixels) { min=Math.min(min,pixel&0xffffff); max=Math.max(max,pixel&0xffffff); }
            if (min==max) throw new AssertionError("Blank portrait "+id);
        }
        byte[] source=Files.readAllBytes(new File(game,"Media/Disp/charicon/char00.dat").toPath());
        rejected(new byte[16]);
        byte[] corrupt=source.clone(); ByteBuffer.wrap(corrupt).order(ByteOrder.LITTLE_ENDIAN).putInt(16,Integer.MAX_VALUE); rejected(corrupt);
        corrupt=source.clone(); ByteBuffer.wrap(corrupt).order(ByteOrder.LITTLE_ENDIAN).putInt(0x40+20,65536); rejected(corrupt);
        corrupt=source.clone(); ByteBuffer.wrap(corrupt).order(ByteOrder.LITTLE_ENDIAN).putInt(0x20+24,16); rejected(corrupt);
        System.out.println("PASS: 24 portraits match original XBE tables; malformed textures rejected");
    }
}
