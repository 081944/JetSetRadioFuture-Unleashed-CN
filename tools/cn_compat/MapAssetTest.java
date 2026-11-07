package com.jumatt.jsrfunleashed;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;

/** Integration check against the user's original game assets; no bundled game data. */
public final class MapAssetTest {
    public static void main(String[] args) throws Exception {
        File disp=new File(args[0],"Media/Disp"), maps=MapAsset.find(disp,"map");
        int count=0, triangles=0;
        for (int group=0;group<=5;++group) {
            MapAsset source=new MapAsset(MapAsset.find(maps,String.format("Stage%02d.dat",group)));
            // Stage00 also contains the world-selection UI after its map models.
            for (int i=2;i<Math.min(12,source.modelCount());++i) {
                MapAsset.Model m=source.model(i); if (m==null) continue;
                if (m.count%3!=0 || m.count<=0) throw new AssertionError("Invalid map triangles");
                for (int k=0;k<3;++k) if (!Float.isFinite(m.min[k]) || m.max[k]<m.min[k]) throw new AssertionError("Invalid bounds");
                ++count; triangles+=m.count/3;
            }
        }
        MapAsset sprites=new MapAsset(MapAsset.find(disp,"SprNorm.dat"));
        MapAsset.Model example=new MapAsset(MapAsset.find(maps,"Stage01.dat")).model(2);
        if (example.count!=2256 || example.opaqueCount!=1128)
            throw new AssertionError("Invisible roots and translucent shell must preserve original alpha");
        for (int v=0;v<example.count;++v) {
            float expected=v<example.opaqueCount ? 1 : 63f/255;
            if (Math.abs(example.vertices.get(v*10+9)-expected)>1e-6f)
                throw new AssertionError("Wrong original map diffuse alpha");
        }
        java.util.Set<String> colors=new java.util.HashSet<>();
        for (int v=0;v<example.count;++v) colors.add(example.vertices.get(v*10+6)+","+example.vertices.get(v*10+7)+","+example.vertices.get(v*10+8));
        System.out.println("Shibuya palette: "+colors);
        int icons=0;
        for (int kind=0;kind<35;++kind) if (sprites.model(kind<12 ? kind+108 : kind+178)!=null) ++icons;
        int names=0;
        for (int kind=12;kind<35;++kind) {
            boolean original=sprites.model(kind+178)!=null;
            if (original!=(MapPlaceNames.forKind(kind)!=null))
                throw new AssertionError("Place name must match original populated slot: "+kind);
            if (original) ++names;
        }
        if (names!=16 || MapPlaceNames.forKind(-1)!=null || MapPlaceNames.forKind(35)!=null)
            throw new AssertionError("Incorrect place-name coverage");
        if (icons<20 || sprites.model(108)==null || sprites.model(111)==null || sprites.model(212)==null)
            throw new AssertionError("Missing map symbols");
        File broken=File.createTempFile("jsrf-bad-map-",".dat");
        try {
            Files.write(broken.toPath(),new byte[64]);
            try { new MapAsset(broken); throw new AssertionError("Accepted malformed chunk"); }
            catch (IOException expected) { }
        } finally { Files.deleteIfExists(broken.toPath()); }
        System.out.println("PASS: "+count+" original maps, "+triangles+" triangles, "+icons+" map symbols; malformed input rejected");
    }
}
